// Lottie (Bodymovin JSON) import (C.13): Lottie.h.
//
// The format, as Bodymovin 5.x writes it (lottie-web reads it the same way):
//   root      { v, fr, ip, op, w, h, nm, ddd, assets[], layers[], fonts{list[]}, markers[] }
//   layer     { ty (0 precomp, 1 solid, 2 image, 3 null, 4 shape, 5 text), ind, parent, nm, ip, op, st, sr, hd, bm,
//               ddd, ao, tt/td (track mattes), ks (transform), masksProperties[], ef[], refId, w, h (precomp),
//               sc, sw, sh (solid), shapes[] (shape), t { d { k[] }, a[], p } (text), tm (time remap) }
//   transform { a, p (or split: { s: true, x, y }), s, r (or rz), o, sk, sa }
//   property  { a: 0, k: value } or { a: 1, k: [ { t, s, e (before 5.5), h, o {x, y}, i {x, y}, ti, to } ] }:
//             key n's o and i shape the segment from key n to key n + 1 (cubic easing in the unit square).
//   shapes    gr { it[] (the last one a tr) }, rc { p, s, r }, el { p, s }, sr { sy, pt, p, r, or, ir, os, is },
//             sh { ks: { c, v[], i[], o[] } }, fl { c, o }, st { c, o, w, lc, lj, d[] }, gf/gs { t, s, e, g { p, k } },
//             tm { s, e, o, m }, rd { r }, tr; the first item draws on top, and a fill or stroke paints the
//             geometry above it in its group (and in groups above it).
// Times: a layer's keys are in its own time, comp time - st; a precomp's comp time is (layer time) / sr.

#include <wabcut/interchange/Lottie.h>

#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wabcut::interchange {

namespace {

using Json = nlohmann::json;

constexpr double kMaxFrames = 1e7;                 ///< |frame| of any time in the file
constexpr double kMaxSeconds = 1e6;                ///< graphic times, well inside the longest timeline
constexpr std::size_t kMaxKeysPerProperty = 10'000;
constexpr std::size_t kMaxTotalKeys = 500'000;     ///< scalar keys over the whole import (precomps repeat theirs)
constexpr std::size_t kMaxTotalVertices = 1'000'000; ///< path vertices over all keys of all paths
constexpr std::size_t kMaxWork = 20'000'000;       ///< layers, shape items, keys and vertices visited, in all
constexpr std::size_t kMaxReportEntries = 500;
constexpr std::size_t kMaxNameBytes = 256;
constexpr double kDefaultAscent = 0.75;            ///< of the font size, when the font list has none

/// A hard error: the import fails with a ParseError.
struct Refused {
    std::string message;
};

// ---- JSON access ------------------------------------------------------------------------------------------------------

const Json* member(const Json& j, const char* key) {
    if (!j.is_object()) {
        return nullptr;
    }
    const auto it = j.find(key);
    return it == j.end() || it->is_null() ? nullptr : &*it;
}

std::optional<double> number(const Json* j) {
    if (!j || !j->is_number()) {
        return std::nullopt;
    }
    const double v = j->get<double>();
    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

double numberOr(const Json& j, const char* key, double fallback) { return number(member(j, key)).value_or(fallback); }

/// An enumeration-like number (types, modes, caps): out-of-range values read as -1, never cast out of int's range.
int intOr(const Json& j, const char* key, int fallback) {
    const double v = numberOr(j, key, fallback);
    return v >= -1e6 && v <= 1e6 ? static_cast<int>(std::lround(v)) : -1;
}

std::string stringOf(const Json& j, const char* key) {
    const Json* m = member(j, key);
    return m && m->is_string() ? m->get<std::string>() : std::string();
}

/// Lottie booleans are true/false or 1/0.
bool flag(const Json& j, const char* key) {
    const Json* m = member(j, key);
    if (!m) {
        return false;
    }
    if (m->is_boolean()) {
        return m->get<bool>();
    }
    const auto v = number(m);
    return v && *v != 0.0;
}

const Json* array(const Json& j, const char* key) {
    const Json* m = member(j, key);
    return m && m->is_array() ? m : nullptr;
}

/// UTF-8 text cut to at most `max` bytes at a code point boundary, without NULs.
std::string bounded(std::string text, std::size_t max) {
    std::erase(text, '\0');
    if (text.size() > max) {
        std::size_t n = max;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) {
            --n;
        }
        text.resize(n);
    }
    return text;
}

/// Numbers of a value: a number, or an array of numbers (non-numbers read as 0).
std::vector<double> numbers(const Json* j) {
    std::vector<double> out;
    if (!j) {
        return out;
    }
    if (j->is_number()) {
        if (const auto v = number(j)) {
            out.push_back(*v);
        }
        return out;
    }
    if (j->is_array()) {
        for (std::size_t i = 0; i < j->size() && i < 64; ++i) {
            out.push_back(number(&(*j)[i]).value_or(0.0));
        }
    }
    return out;
}

double at(const std::vector<double>& v, std::size_t i, double fallback) {
    if (i < v.size()) {
        return v[i];
    }
    return v.size() == 1 ? v[0] : fallback;
}

// ---- Time -------------------------------------------------------------------------------------------------------------

/// Root frames from local frames: root = offset + scale * local.
struct TimeMap {
    double offset = 0.0;
    double scale = 1.0;

    double operator()(double local) const { return offset + scale * local; }
};

// ---- Raw properties ---------------------------------------------------------------------------------------------------

struct Ease {
    std::vector<double> x;
    std::vector<double> y;
};

struct RawKey {
    double frame = 0.0; ///< root frames
    std::vector<double> value;
    bool hold = false;
    Ease out; ///< this key's o: the start of the segment to the next key
    Ease in;  ///< this key's i: the end of that segment
};

struct RawProp {
    bool present = false;
    bool animated = false;
    bool expression = false;
    bool spatial = false; ///< position keys with motion path tangents
    std::vector<double> value;
    std::vector<RawKey> keys;
};

Ease readEase(const Json* e) {
    Ease out;
    if (e && e->is_object()) {
        out.x = numbers(member(*e, "x"));
        out.y = numbers(member(*e, "y"));
    }
    return out;
}

bool isLinear(const Ease& out, const Ease& in, std::size_t dim) {
    if (out.x.empty() || in.x.empty()) {
        return true;
    }
    const double ox = at(out.x, dim, 0.0);
    const double oy = at(out.y, dim, ox);
    const double ix = at(in.x, dim, 1.0);
    const double iy = at(in.y, dim, ix);
    return std::abs(ox - oy) < 1e-9 && std::abs(ix - iy) < 1e-9;
}

bool keyframed(const Json& k) { return k.is_array() && !k.empty() && k[0].is_object() && k[0].contains("t"); }

// ---- The importer -----------------------------------------------------------------------------------------------------

struct Content {
    double x = 0.0; ///< where the Wabcut layer's origin is in the Lottie layer's space
    double y = 0.0;
};

/// A style item (fill, stroke, gradient, trim, round corners) and the group it came from, nearest first.
struct StyleRef {
    const Json* item = nullptr;
    std::string type;
    std::string where;
};

class Importer {
public:
    Importer(const Json& root, std::string fileName) : m_root(root), m_file(std::move(fileName)) {}

    LottieImport run() {
        readRoot();
        LottieImport out;
        out.frameRate = m_rate;
        out.duration = m_duration;
        out.graphic.name = bounded(stringOf(m_root, "nm"), kMaxNameBytes);
        out.graphic.width = m_width;
        out.graphic.height = m_height;
        const Json* layers = array(m_root, "layers");
        std::vector<std::string> stack;
        out.graphic.layers = convertComp(*layers, TimeMap{}, 1, stack, "/layers");
        if (const Json* markers = array(m_root, "markers"); markers && !markers->empty()) {
            note(ReportOutcome::Dropped, ReportCategory::Marker,
                 std::to_string(markers->size()) + " composition marker(s) are not imported", "/markers");
        }
        if (flag(m_root, "ddd")) {
            note(ReportOutcome::Approximated, ReportCategory::Graphic, "a 3D composition is imported flat", "/ddd");
        }
        if (m_suppressed > 0) {
            m_report.add(ReportOutcome::Dropped, ReportCategory::Graphic,
                         std::to_string(m_suppressed) + " more report entries are not listed", {m_file, "/", 0});
        }
        out.report = std::move(m_report);
        return out;
    }

private:
    // ---- Root -------------------------------------------------------------------------------------------------------

    void readRoot() {
        if (!m_root.is_object() || !array(m_root, "layers") || !member(m_root, "fr")) {
            throw Refused{"not a Lottie animation (no frame rate or layers)"};
        }
        const auto fr = number(member(m_root, "fr"));
        if (!fr || *fr <= 0.0 || *fr > 1000.0) {
            throw Refused{"the frame rate must be 0..1000 frames per second"};
        }
        m_fr = *fr;
        const auto ip = number(member(m_root, "ip"));
        const auto op = number(member(m_root, "op"));
        if (!ip || !op || std::abs(*ip) > kMaxFrames || std::abs(*op) > kMaxFrames || *op <= *ip) {
            throw Refused{"the animation needs an in point (ip) before its out point (op)"};
        }
        m_ip = *ip;
        m_duration = ticks(*op).value_or(Ticks{});
        if (m_duration.value <= 0) {
            throw Refused{"the animation is too long"};
        }
        const auto w = number(member(m_root, "w"));
        const auto h = number(member(m_root, "h"));
        if (!w || !h || *w < 1.0 || *h < 1.0 || *w > kMaxGraphicCanvas || *h > kMaxGraphicCanvas) {
            throw Refused{"the canvas (w x h) must be 1.." + std::to_string(kMaxGraphicCanvas) + " pixels"};
        }
        m_width = static_cast<std::int32_t>(std::lround(*w));
        m_height = static_cast<std::int32_t>(std::lround(*h));
        m_rate = rateOf(m_fr);

        if (const Json* assets = array(m_root, "assets")) {
            if (assets->size() > kMaxLottieAssets) {
                throw Refused{"more than " + std::to_string(kMaxLottieAssets) + " assets"};
            }
            for (std::size_t i = 0; i < assets->size(); ++i) {
                const Json& a = (*assets)[i];
                const std::string id = stringOf(a, "id");
                if (!id.empty()) {
                    m_assets.emplace(id, std::pair{&a, i});
                }
            }
        }
        if (const Json* fonts = member(m_root, "fonts")) {
            if (const Json* list = array(*fonts, "list")) {
                for (std::size_t i = 0; i < list->size() && i < 1000; ++i) {
                    const Json& f = (*list)[i];
                    m_fonts[stringOf(f, "fName")] = &f;
                }
            }
        }
    }

    static Rate rateOf(double fr) {
        // NTSC rates (29.97 is 30000/1001), then whole, hundredths and thousandths of frames.
        const double ntsc = std::round(fr * 1.001);
        if (ntsc >= 1.0 && std::abs(fr - ntsc * 1000.0 / 1001.0) < 0.002 && std::abs(fr - std::round(fr)) > 0.01) {
            if (const auto r = Rate::make(std::llround(ntsc) * 1000, 1001)) {
                return *r;
            }
        }
        for (const std::int64_t den : {1, 100, 1000}) {
            const double num = fr * static_cast<double>(den);
            if (std::abs(num - std::round(num)) < 1e-6 * static_cast<double>(den)) {
                if (const auto r = Rate::make(std::llround(num), den)) {
                    return *r;
                }
            }
        }
        return Rate::make(std::llround(fr * 1000.0), 1000).value_or(Rate{30, 1});
    }

    /// Graphic time of a root frame; nullopt beyond the bounds.
    std::optional<Ticks> ticks(double rootFrame) const {
        const double seconds = (rootFrame - m_ip) / m_fr;
        if (!std::isfinite(seconds) || std::abs(seconds) > kMaxSeconds) {
            return std::nullopt;
        }
        return Ticks{std::llround(seconds * static_cast<double>(kTicksPerSecond))};
    }

    // ---- Reporting --------------------------------------------------------------------------------------------------

    void note(ReportOutcome outcome, ReportCategory category, std::string reason, const std::string& where) {
        if (outcome != ReportOutcome::Carried) {
            ++m_layerNotes;
        }
        if (m_report.size() >= kMaxReportEntries) {
            ++m_suppressed;
            return;
        }
        m_report.add(outcome, category, std::move(reason), SourceLocation{m_file, where, 0});
    }
    void approximated(std::string reason, const std::string& where) {
        note(ReportOutcome::Approximated, ReportCategory::Graphic, std::move(reason), where);
    }
    void dropped(std::string reason, const std::string& where, ReportCategory category = ReportCategory::Graphic) {
        note(ReportOutcome::Dropped, category, std::move(reason), where);
    }

    /// Reserves one Wabcut layer; false (reported once) when the graphic is full.
    bool takeLayer(const std::string& where) {
        if (m_layerCount >= kMaxGraphicLayers) {
            if (!m_fullReported) {
                m_fullReported = true;
                dropped("the graphic holds at most " + std::to_string(kMaxGraphicLayers) +
                            " layers: the layers from here on are not imported",
                        where);
            }
            return false;
        }
        ++m_layerCount;
        return true;
    }

    std::uint32_t nextId() { return ++m_lastId; }

    void work(std::size_t units = 1) {
        m_work += units;
        if (m_work > kMaxWork) {
            throw Refused{"the animation is too complex to import"};
        }
    }

    // ---- Properties -------------------------------------------------------------------------------------------------

    RawProp readProp(const Json* prop, const TimeMap& time, const std::string& where) {
        RawProp out;
        if (!prop) {
            return out;
        }
        out.present = true;
        if (!prop->is_object()) {
            out.value = numbers(prop);
            return out;
        }
        if (const Json* x = member(*prop, "x"); x && x->is_string() && !x->get_ref<const std::string&>().empty()) {
            out.expression = true;
            approximated("an expression is ignored: its keyframes or value are used", where);
        }
        const Json* k = member(*prop, "k");
        if (!k) {
            out.present = false;
            return out;
        }
        if (!keyframed(*k)) {
            out.value = numbers(k);
            return out;
        }
        out.animated = true;
        const std::size_t n = std::min(k->size(), kMaxKeysPerProperty);
        if (k->size() > kMaxKeysPerProperty) {
            approximated("more than " + std::to_string(kMaxKeysPerProperty) + " keyframes: the rest are dropped", where);
        }
        std::vector<double> previousEnd;
        for (std::size_t i = 0; i < n; ++i) {
            const Json& key = (*k)[i];
            work();
            const auto t = number(member(key, "t"));
            if (!t || std::abs(*t) > kMaxFrames) {
                continue;
            }
            RawKey rk;
            rk.frame = time(*t);
            std::vector<double> s = numbers(member(key, "s"));
            if (s.empty()) {
                s = previousEnd.empty() && !out.keys.empty() ? out.keys.back().value : previousEnd;
            }
            if (s.empty()) {
                continue;
            }
            rk.value = std::move(s);
            previousEnd = numbers(member(key, "e"));
            rk.hold = flag(key, "h");
            rk.out = readEase(member(key, "o"));
            rk.in = readEase(member(key, "i"));
            for (const char* tangent : {"ti", "to"}) {
                for (const double v : numbers(member(key, tangent))) {
                    if (v != 0.0) {
                        out.spatial = true;
                    }
                }
            }
            out.keys.push_back(std::move(rk));
        }
        if (out.keys.empty()) {
            out.animated = false;
            out.present = false;
        }
        return out;
    }

    /// One dimension of a property as a ParamValue: value * scale + add, kept in [lo, hi]; easing as Bezier handles.
    ParamValue param(const RawProp& p, std::size_t dim, double fallback, double scale, double add, double lo, double hi,
                     const std::string& where) {
        bool clamped = false;
        const auto fit = [&](double v) {
            const double scaled = v * scale + add;
            const double c = std::clamp(std::isfinite(scaled) ? scaled : fallback, lo, hi);
            if (c != scaled) {
                clamped = true;
            }
            return c;
        };
        ParamValue result(std::clamp(fallback, lo, hi));
        if (p.present && !p.animated) {
            result = ParamValue(fit(at(p.value, dim, (fallback - add) / (scale == 0.0 ? 1.0 : scale))));
        } else if (p.animated) {
            // Times and values first; keys whose time does not move forward are dropped.
            struct Point {
                Ticks t;
                double v;
                const RawKey* raw;
            };
            std::vector<Point> points;
            bool skipped = false;
            for (const RawKey& k : p.keys) {
                const auto t = ticks(k.frame);
                if (!t || (!points.empty() && *t <= points.back().t)) {
                    skipped = true;
                    continue;
                }
                points.push_back({*t, fit(at(k.value, dim, (fallback - add) / (scale == 0.0 ? 1.0 : scale))), &k});
            }
            if (skipped) {
                approximated("keyframes out of time order or beyond the timeline are dropped", where);
            }
            if (m_totalKeys + points.size() > kMaxTotalKeys) {
                approximated("too many keyframes in the animation: this property keeps its first value", where);
                points.resize(points.empty() ? 0 : 1);
            }
            m_totalKeys += points.size();
            const bool flat = std::all_of(points.begin(), points.end(), [&](const Point& q) { return q.v == points.front().v; });
            if (points.size() == 1 || (flat && !points.empty())) {
                result = ParamValue(points[0].v); // keys that never change the value: a constant
            } else if (points.size() > 1) {
                Curve curve;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    Keyframe key;
                    key.time = points[i].t;
                    key.value = points[i].v;
                    const RawKey& raw = *points[i].raw;
                    const bool hasNext = i + 1 < points.size();
                    const bool hasPrevious = i > 0;
                    const bool leavingLinear = !hasNext || raw.hold || isLinear(raw.out, raw.in, dim);
                    const bool arrivingLinear =
                        !hasPrevious || points[i - 1].raw->hold || isLinear(points[i - 1].raw->out, points[i - 1].raw->in, dim);
                    if (raw.hold && hasNext) {
                        key.interpolation = Interpolation::Hold;
                    } else if (leavingLinear && arrivingLinear) {
                        key.interpolation = Interpolation::Linear;
                    } else {
                        key.interpolation = Interpolation::Bezier;
                        if (hasNext) {
                            key.out = handle(raw.out, dim, points[i], points[i + 1], true, leavingLinear);
                        }
                        if (hasPrevious) {
                            key.in = handle(points[i - 1].raw->in, dim, points[i - 1], points[i], false, arrivingLinear);
                        }
                    }
                    curve.keys.push_back(key);
                }
                result = ParamValue(std::move(curve));
            }
        }
        if (clamped) {
            approximated("values outside " + range(lo, hi) + " are clamped", where);
        }
        return result;
    }

    template <class Point>
    static BezierHandle handle(const Ease& e, std::size_t dim, const Point& a, const Point& b, bool leaving, bool linear) {
        const double seconds = static_cast<double>((b.t - a.t).value) / static_cast<double>(kTicksPerSecond);
        const double dv = b.v - a.v;
        if (linear || e.x.empty()) {
            return {dv / seconds, 1.0 / 3.0};
        }
        const double x = std::clamp(at(e.x, dim, leaving ? 0.0 : 1.0), 0.0, 1.0);
        const double y = at(e.y, dim, x);
        // Leaving: the control point (x, y) of the unit square; arriving: (x, y) measured back from (1, 1).
        const double influence = std::max(leaving ? x : 1.0 - x, 1e-6);
        const double rise = leaving ? y : 1.0 - y;
        const double slope = rise * dv / (influence * seconds);
        return {std::isfinite(slope) ? std::clamp(slope, -1e12, 1e12) : 0.0, influence};
    }

    static std::string range(double lo, double hi) {
        const auto fmt = [](double v) {
            std::string s = std::to_string(v);
            s.erase(s.find_last_not_of('0') + 1);
            if (!s.empty() && s.back() == '.') {
                s.pop_back();
            }
            return s;
        };
        return fmt(lo) + ".." + fmt(hi);
    }

    /// The first value of a property (animated ones: their first key), for things the model holds as constants.
    std::vector<double> firstValue(const RawProp& p, const std::string& where, const char* what) {
        if (p.animated) {
            approximated(std::string("an animated ") + what + " keeps its first value", where);
            return p.keys.front().value;
        }
        return p.value;
    }

    // ---- Transforms -------------------------------------------------------------------------------------------------

    LayerTransform transformOf(const Json* tr, const TimeMap& time, Content content, bool withOpacity, const std::string& where) {
        LayerTransform out;
        if (!tr || !tr->is_object()) {
            out.anchorX = ParamValue(-content.x);
            out.anchorY = ParamValue(-content.y);
            return out;
        }
        constexpr double kCoord = kMaxGraphicCoordinate;
        const RawProp anchor = readProp(member(*tr, "a"), time, where + "/a");
        out.anchorX = param(anchor, 0, 0.0, 1.0, -content.x, -kCoord, kCoord, where + "/a");
        out.anchorY = param(anchor, 1, 0.0, 1.0, -content.y, -kCoord, kCoord, where + "/a");
        if (const Json* p = member(*tr, "p"); p && p->is_object() && flag(*p, "s")) {
            out.x = param(readProp(member(*p, "x"), time, where + "/p/x"), 0, 0.0, 1.0, 0.0, -kCoord, kCoord, where + "/p/x");
            out.y = param(readProp(member(*p, "y"), time, where + "/p/y"), 0, 0.0, 1.0, 0.0, -kCoord, kCoord, where + "/p/y");
        } else {
            const RawProp position = readProp(p, time, where + "/p");
            if (position.spatial) {
                approximated("a curved motion path moves in straight lines between its keyframes", where + "/p");
            }
            out.x = param(position, 0, 0.0, 1.0, 0.0, -kCoord, kCoord, where + "/p");
            out.y = param(position, 1, 0.0, 1.0, 0.0, -kCoord, kCoord, where + "/p");
        }
        const RawProp scale = readProp(member(*tr, "s"), time, where + "/s");
        out.scaleX = param(scale, 0, 100.0, 1.0, 0.0, -100000.0, 100000.0, where + "/s");
        out.scaleY = param(scale, 1, 100.0, 1.0, 0.0, -100000.0, 100000.0, where + "/s");
        const Json* rotation = member(*tr, "r");
        if (!rotation) {
            rotation = member(*tr, "rz");
        }
        out.rotation = param(readProp(rotation, time, where + "/r"), 0, 0.0, 1.0, 0.0, -1e6, 1e6, where + "/r");
        if (withOpacity) {
            out.opacity = param(readProp(member(*tr, "o"), time, where + "/o"), 0, 100.0, 1.0, 0.0, 0.0, 100.0, where + "/o");
        }
        for (const char* key : {"sk", "rx", "ry"}) {
            const RawProp p = readProp(member(*tr, key), time, where + "/" + key);
            const bool nonZero = p.animated || std::any_of(p.value.begin(), p.value.end(), [](double v) { return v != 0.0; });
            if (p.present && nonZero) {
                approximated(std::string(key) == "sk" ? "skew is not imported" : "3D rotation is not imported", where + "/" + key);
            }
        }
        if (const RawProp orientation = readProp(member(*tr, "or"), time, where + "/or");
            orientation.present &&
            (orientation.animated || std::any_of(orientation.value.begin(), orientation.value.end(), [](double v) { return v != 0.0; }))) {
            approximated("3D orientation is not imported", where + "/or");
        }
        return out;
    }

    // ---- Paths ------------------------------------------------------------------------------------------------------

    struct PathShape {
        bool closed = false;
        std::vector<MaskVertex> vertices;
    };

    std::optional<PathShape> pathValue(const Json* v, double sx, double sy, double ox, double oy) {
        if (v && v->is_array() && !v->empty()) {
            v = &(*v)[0]; // keyed values hold the shape in a one-element array
        }
        if (!v || !v->is_object()) {
            return std::nullopt;
        }
        const Json* points = array(*v, "v");
        if (!points) {
            return std::nullopt;
        }
        const Json* ins = array(*v, "i");
        const Json* outs = array(*v, "o");
        PathShape shape;
        shape.closed = flag(*v, "c");
        const std::size_t n = points->size();
        if (n > kMaxPathVertices) {
            return std::nullopt;
        }
        work(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::vector<double> p = numbers(&(*points)[i]);
            const std::vector<double> in = ins && i < ins->size() ? numbers(&(*ins)[i]) : std::vector<double>{};
            const std::vector<double> out = outs && i < outs->size() ? numbers(&(*outs)[i]) : std::vector<double>{};
            MaskVertex vertex{(at(p, 0, 0.0) - ox) * sx, (at(p, 1, 0.0) - oy) * sy, at(in, 0, 0.0) * sx,
                              at(in, 1, 0.0) * sy,      at(out, 0, 0.0) * sx,      at(out, 1, 0.0) * sy};
            for (const double c : {vertex.x, vertex.y, vertex.inX, vertex.inY, vertex.outX, vertex.outY}) {
                if (!std::isfinite(c) || std::abs(c) > kMaxGraphicCoordinate) {
                    return std::nullopt;
                }
            }
            shape.vertices.push_back(vertex);
        }
        return shape;
    }

    /// A path property as keys: pixels relative to (ox, oy), scaled by (sx, sy) (masks: normalised by the canvas).
    std::optional<std::pair<bool, std::vector<MaskPathKey>>> pathKeys(const Json* prop, const TimeMap& time, double sx, double sy,
                                                                      double ox, double oy, const std::string& where) {
        if (!prop || !prop->is_object()) {
            return std::nullopt;
        }
        if (const Json* x = member(*prop, "x"); x && x->is_string() && !x->get_ref<const std::string&>().empty()) {
            approximated("an expression is ignored: its keyframes or value are used", where);
        }
        const Json* k = member(*prop, "k");
        if (!k) {
            return std::nullopt;
        }
        std::vector<MaskPathKey> keys;
        bool closed = false;
        if (!keyframed(*k)) {
            const auto shape = pathValue(k, sx, sy, ox, oy);
            if (!shape) {
                return std::nullopt;
            }
            closed = shape->closed;
            keys.push_back(MaskPathKey{Ticks{}, shape->vertices, Interpolation::Linear});
        } else {
            const std::size_t n = std::min(k->size(), kMaxKeysPerProperty);
            bool eased = false;
            bool skipped = false;
            const Json* previousEnd = nullptr;
            for (std::size_t i = 0; i < n; ++i) {
                const Json& key = (*k)[i];
                const auto t = number(member(key, "t"));
                const auto when = t && std::abs(*t) <= kMaxFrames ? ticks(time(*t)) : std::nullopt;
                const Json* s = member(key, "s");
                if (!s) {
                    s = previousEnd;
                }
                previousEnd = member(key, "e");
                const auto shape = pathValue(s, sx, sy, ox, oy);
                if (!when || !shape || (!keys.empty() && *when <= keys.back().time)) {
                    skipped = true;
                    continue;
                }
                if (!keys.empty() && shape->vertices.size() != keys.front().vertices.size()) {
                    approximated("path keyframes with different vertex counts: the path keeps its first shape", where);
                    keys.resize(1);
                    break;
                }
                if (m_totalVertices + shape->vertices.size() > kMaxTotalVertices) {
                    approximated("too many path vertices in the animation: the rest of this path's keyframes are dropped", where);
                    break;
                }
                m_totalVertices += shape->vertices.size();
                if (keys.empty()) {
                    closed = shape->closed;
                }
                Interpolation interpolation = Interpolation::Linear;
                if (flag(key, "h")) {
                    interpolation = Interpolation::Hold;
                } else if (!isLinear(readEase(member(key, "o")), readEase(member(key, "i")), 0)) {
                    interpolation = Interpolation::Bezier;
                    eased = true;
                }
                keys.push_back(MaskPathKey{*when, shape->vertices, interpolation});
            }
            if (eased) {
                approximated("path keyframe easing is approximated by ease in and out", where);
            }
            if (skipped) {
                approximated("path keyframes out of time order, beyond the timeline or malformed are dropped", where);
            }
        }
        if (keys.empty()) {
            return std::nullopt;
        }
        return std::pair{closed, std::move(keys)};
    }

    // ---- Paints -----------------------------------------------------------------------------------------------------

    GraphicColor colorOf(const RawProp& c, const RawProp& opacity, const std::string& where) {
        const std::vector<double> rgba = firstValue(c, where + "/c", "colour");
        const std::vector<double> o = firstValue(opacity, where + "/o", "opacity");
        GraphicColor color;
        for (std::size_t i = 0; i < 3; ++i) {
            double v = at(rgba, i, 1.0);
            if (v > 1.0 && std::all_of(rgba.begin(), rgba.end(), [](double x) { return x <= 255.0; }) &&
                std::any_of(rgba.begin(), rgba.end(), [](double x) { return x > 1.0; })) {
                v /= 255.0; // very old exports wrote 0..255
            }
            color.rgba[i] = std::clamp(v, 0.0, 1.0);
        }
        const double alpha = rgba.size() >= 4 ? std::clamp(rgba[3], 0.0, 1.0) : 1.0;
        color.rgba[3] = std::clamp(alpha * at(o, 0, 100.0) / 100.0, 0.0, 1.0);
        return color;
    }

    static std::optional<std::array<double, 4>> hexColor(const std::string& s) {
        if (s.size() != 7 || s[0] != '#') {
            return std::nullopt;
        }
        std::array<double, 4> out{0.0, 0.0, 0.0, 1.0};
        for (std::size_t i = 0; i < 3; ++i) {
            int v = 0;
            for (std::size_t j = 0; j < 2; ++j) {
                const char c = s[1 + 2 * i + j];
                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) {
                    return std::nullopt;
                }
                v = v * 16 + d;
            }
            out[i] = v / 255.0;
        }
        return out;
    }

    Paint fillOf(const StyleRef& s, double ox, double oy, const TimeMap& time) {
        Paint paint;
        const Json& item = *s.item;
        const RawProp opacity = readProp(member(item, "o"), time, s.where + "/o");
        if (s.type == "fl") {
            paint.kind = PaintKind::Solid;
            paint.color = colorOf(readProp(member(item, "c"), time, s.where + "/c"), opacity, s.where);
            return paint;
        }
        // Gradient fill.
        paint.kind = numberOr(item, "t", 1.0) == 2.0 ? PaintKind::Radial : PaintKind::Linear;
        const std::vector<double> start = firstValue(readProp(member(item, "s"), time, s.where + "/s"), s.where + "/s", "gradient start");
        const std::vector<double> end = firstValue(readProp(member(item, "e"), time, s.where + "/e"), s.where + "/e", "gradient end");
        const auto coord = [](double v) { return std::clamp(v, -kMaxGraphicCoordinate, kMaxGraphicCoordinate); };
        paint.start = {coord(at(start, 0, 0.0) - ox), coord(at(start, 1, 0.0) - oy)};
        paint.end = {coord(at(end, 0, 100.0) - ox), coord(at(end, 1, 0.0) - oy)};
        const double layerAlpha = std::clamp(at(firstValue(opacity, s.where + "/o", "opacity"), 0, 100.0) / 100.0, 0.0, 1.0);
        if (paint.kind == PaintKind::Radial) {
            for (const char* key : {"h", "a"}) {
                const RawProp p = readProp(member(item, key), time, s.where + "/" + key);
                if (p.present && (p.animated || at(p.value, 0, 0.0) != 0.0)) {
                    approximated("a radial gradient's highlight is centred", s.where + "/" + key);
                    break;
                }
            }
        }
        const Json* g = member(item, "g");
        std::size_t count = g ? static_cast<std::size_t>(std::clamp(numberOr(*g, "p", 0.0), 0.0, 1000.0)) : 0;
        const std::vector<double> data = g ? firstValue(readProp(member(*g, "k"), time, s.where + "/g"), s.where + "/g", "gradient")
                                           : std::vector<double>{};
        if (count > kMaxGradientStops) {
            approximated("a gradient keeps its first " + std::to_string(kMaxGradientStops) + " colour stops", s.where + "/g");
        }
        count = std::min({count, kMaxGradientStops, data.size() / 4});
        // Opacity stops follow the colour stops as (offset, alpha) pairs; the alpha at each colour stop is interpolated.
        std::vector<std::pair<double, double>> alphas;
        for (std::size_t i = 4 * count; g && i + 1 < data.size() && alphas.size() < 64; i += 2) {
            alphas.emplace_back(data[i], data[i + 1]);
        }
        const auto alphaAt = [&](double offset) {
            if (alphas.empty()) {
                return 1.0;
            }
            if (offset <= alphas.front().first) {
                return alphas.front().second;
            }
            for (std::size_t i = 1; i < alphas.size(); ++i) {
                if (offset <= alphas[i].first) {
                    const auto [o0, a0] = alphas[i - 1];
                    const auto [o1, a1] = alphas[i];
                    return o1 > o0 ? a0 + (a1 - a0) * (offset - o0) / (o1 - o0) : a1;
                }
            }
            return alphas.back().second;
        };
        double last = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            GradientStop stop;
            stop.offset = std::clamp(data[4 * i], last, 1.0);
            last = stop.offset;
            for (std::size_t c = 0; c < 3; ++c) {
                stop.color.rgba[c] = std::clamp(data[4 * i + 1 + c], 0.0, 1.0);
            }
            stop.color.rgba[3] = std::clamp(alphaAt(stop.offset) * layerAlpha, 0.0, 1.0);
            paint.stops.push_back(stop);
        }
        if (paint.stops.size() < 2) {
            approximated("a gradient without two colour stops is drawn in one colour", s.where);
            paint.kind = PaintKind::Solid;
            paint.color = paint.stops.empty() ? GraphicColor{} : paint.stops.front().color;
            paint.stops.clear();
        }
        return paint;
    }

    Stroke strokeOf(const StyleRef& s, const TimeMap& time) {
        Stroke stroke;
        stroke.enabled = true;
        const Json& item = *s.item;
        const RawProp opacity = readProp(member(item, "o"), time, s.where + "/o");
        if (s.type == "gs") {
            approximated("a gradient stroke is drawn in its first colour", s.where);
            StyleRef asFill = s;
            asFill.type = "gf";
            const Paint p = fillOf(asFill, 0.0, 0.0, time);
            stroke.color = p.kind == PaintKind::Solid ? p.color : p.stops.front().color;
        } else {
            stroke.color = colorOf(readProp(member(item, "c"), time, s.where + "/c"), opacity, s.where);
        }
        stroke.width = param(readProp(member(item, "w"), time, s.where + "/w"), 0, 2.0, 1.0, 0.0, 0.0, 1000.0, s.where + "/w");
        const int cap = intOr(item, "lc", 1);
        stroke.cap = cap == 2 ? LineCap::Round : cap == 3 ? LineCap::Square : LineCap::Butt;
        const int join = intOr(item, "lj", 1);
        stroke.join = join == 2 ? LineJoin::Round : join == 3 ? LineJoin::Bevel : LineJoin::Miter;
        if (const Json* dashes = array(item, "d")) {
            for (std::size_t i = 0; i < dashes->size() && i < 64; ++i) {
                const Json& d = (*dashes)[i];
                const std::string kind = stringOf(d, "n");
                const RawProp v = readProp(member(d, "v"), time, s.where + "/d");
                const double value = at(firstValue(v, s.where + "/d", "dash"), 0, 0.0);
                if (kind == "o") {
                    if (value != 0.0) {
                        approximated("a dash offset is not imported", s.where + "/d");
                    }
                } else if (stroke.dashes.size() < kMaxDashes) {
                    stroke.dashes.push_back(std::clamp(value, 0.0, 10000.0));
                }
            }
            if (std::all_of(stroke.dashes.begin(), stroke.dashes.end(), [](double d) { return d == 0.0; })) {
                stroke.dashes.clear();
            }
        }
        return stroke;
    }

    // ---- Shapes -----------------------------------------------------------------------------------------------------

    /// The shapes of a group (`items`, first on top), with the styles of the groups around it (nearest first), as
    /// layers bottom to top. `depth`: where the layers will sit.
    std::vector<GraphicLayer> convertShapes(const Json& items, const std::vector<StyleRef>& inherited, const TimeMap& time,
                                            std::size_t depth, const std::string& where) {
        std::vector<GraphicLayer> out;
        if (!items.is_array()) {
            return out;
        }
        const std::size_t n = std::min<std::size_t>(items.size(), 10'000);
        // Styles below item i in this group (nearest first), then the inherited ones.
        std::vector<StyleRef> below;
        std::vector<std::pair<std::size_t, std::vector<StyleRef>>> order;
        for (std::size_t r = n; r-- > 0;) {
            const Json& item = items[r];
            work();
            const std::string type = stringOf(item, "ty");
            const std::string here = where + "/" + std::to_string(r);
            if (type == "fl" || type == "st" || type == "gf" || type == "gs" || type == "tm" || type == "rd") {
                if (!flag(item, "hd")) {
                    below.insert(below.begin(), StyleRef{&item, type, here});
                }
                continue;
            }
            std::vector<StyleRef> applying = below;
            applying.insert(applying.end(), inherited.begin(), inherited.end());
            order.emplace_back(r, std::move(applying));
        }
        // `order` runs bottom (last item) to top (first item): the layer order.
        for (auto& [index, styles] : order) {
            const Json& item = items[index];
            const std::string type = stringOf(item, "ty");
            const std::string here = where + "/" + std::to_string(index);
            const std::string name = bounded(stringOf(item, "nm"), kMaxNameBytes);
            if (flag(item, "hd") || type == "tr") {
                continue;
            }
            if (type == "gr") {
                if (depth > kMaxGraphicDepth - 1) {
                    dropped("shape groups nested deeper than " + std::to_string(kMaxGraphicDepth) + " levels", here);
                    continue;
                }
                if (!takeLayer(here)) {
                    return out;
                }
                GraphicLayer group;
                group.id = nextId();
                group.kind = LayerKind::Group;
                group.name = name;
                const Json* it = array(item, "it");
                const Json* tr = nullptr;
                if (it) {
                    for (std::size_t i = 0; i < it->size(); ++i) {
                        if (stringOf((*it)[i], "ty") == "tr") {
                            tr = &(*it)[i];
                        }
                    }
                }
                group.transform = transformOf(tr, time, {}, true, here + "/tr");
                if (it) {
                    group.children = convertShapes(*it, styles, time, depth + 1, here + "/it");
                }
                out.push_back(std::move(group));
                continue;
            }
            if (type == "rc" || type == "el" || type == "sr" || type == "sh") {
                if (auto layer = geometry(item, type, name, styles, time, here)) {
                    out.push_back(std::move(*layer));
                }
                continue;
            }
            static const std::map<std::string, std::string> kUnsupported{
                {"mm", "merge paths"}, {"rp", "a repeater"},     {"pb", "pucker and bloat"}, {"tw", "twist"},
                {"op", "offset path"}, {"zz", "zig zag"},        {"ms", "a merge"},          {"no", "a no-op"}};
            const auto known = kUnsupported.find(type);
            dropped((known != kUnsupported.end() ? known->second : "shape item \"" + bounded(type, 16) + "\"") +
                        " is not imported",
                    here);
        }
        return out;
    }

    std::optional<GraphicLayer> geometry(const Json& item, const std::string& type, const std::string& name,
                                         const std::vector<StyleRef>& styles, const TimeMap& time, const std::string& where) {
        // The nearest fill and stroke paint it; without either it does not draw.
        const StyleRef* fill = nullptr;
        const StyleRef* stroke = nullptr;
        const StyleRef* trim = nullptr;
        const StyleRef* round = nullptr;
        std::size_t fills = 0;
        std::size_t strokes = 0;
        std::size_t fillAt = 0;
        std::size_t strokeAt = 0;
        for (std::size_t i = 0; i < styles.size(); ++i) {
            const StyleRef& s = styles[i];
            if (s.type == "fl" || s.type == "gf") {
                if (!fill) {
                    fill = &s;
                    fillAt = i;
                }
                ++fills;
            } else if (s.type == "st" || s.type == "gs") {
                if (!stroke) {
                    stroke = &s;
                    strokeAt = i;
                }
                ++strokes;
            } else if (s.type == "tm") {
                if (trim) {
                    approximated("only the nearest trim paths applies", s.where);
                } else {
                    trim = &s;
                }
            } else if (s.type == "rd" && !round) {
                round = &s;
            }
        }
        if (!fill && !stroke) {
            return std::nullopt;
        }
        if (fills > 1 || strokes > 1) {
            approximated("a shape painted by several fills or strokes keeps the nearest of each", where);
        }
        if (fill && stroke && fillAt < strokeAt) {
            approximated("a fill above its stroke is drawn below it", where);
        }
        if (!takeLayer(where)) {
            return std::nullopt;
        }
        GraphicLayer layer;
        layer.id = nextId();
        layer.kind = LayerKind::Shape;
        layer.name = name;
        ShapeContent& shape = layer.shape;
        constexpr double kCoord = kMaxGraphicCoordinate;
        double ox = 0.0; // the geometry's centre: the layer's origin, for gradients
        double oy = 0.0;
        if (type == "sh") {
            shape.kind = ShapeKind::Path;
            const auto keys = pathKeys(member(item, "ks"), time, 1.0, 1.0, 0.0, 0.0, where + "/ks");
            if (!keys || keys->second.front().vertices.size() < (keys->first ? 3u : 2u)) {
                dropped("a path without enough vertices (or more than " + std::to_string(kMaxPathVertices) + ")", where);
                --m_layerCount;
                return std::nullopt;
            }
            shape.closed = keys->first;
            shape.path = keys->second;
        } else {
            const RawProp position = readProp(member(item, "p"), time, where + "/p");
            layer.transform.x = param(position, 0, 0.0, 1.0, 0.0, -kCoord, kCoord, where + "/p");
            layer.transform.y = param(position, 1, 0.0, 1.0, 0.0, -kCoord, kCoord, where + "/p");
            ox = layer.transform.x.valueAt(Ticks{});
            oy = layer.transform.y.valueAt(Ticks{});
            if (type == "rc" || type == "el") {
                shape.kind = type == "rc" ? ShapeKind::Rectangle : ShapeKind::Ellipse;
                const RawProp size = readProp(member(item, "s"), time, where + "/s");
                shape.width = param(size, 0, 100.0, 1.0, 0.0, 0.0, kCoord, where + "/s");
                shape.height = param(size, 1, 100.0, 1.0, 0.0, 0.0, kCoord, where + "/s");
                if (type == "rc") {
                    shape.cornerRadius = param(readProp(member(item, "r"), time, where + "/r"), 0, 0.0, 1.0, 0.0, 0.0, kCoord, where + "/r");
                    if (round && shape.cornerRadius == ParamValue(0.0)) {
                        shape.cornerRadius = param(readProp(member(*round->item, "r"), time, round->where + "/r"), 0, 0.0, 1.0, 0.0,
                                                   0.0, kCoord, round->where + "/r");
                    }
                } else if (round) {
                    approximated("rounded corners apply to rectangles only", round->where);
                }
            } else { // sr
                shape.kind = numberOr(item, "sy", 1.0) == 2.0 ? ShapeKind::Polygon : ShapeKind::Star;
                const std::vector<double> points =
                    firstValue(readProp(member(item, "pt"), time, where + "/pt"), where + "/pt", "point count");
                const double count = std::round(at(points, 0, 5.0));
                shape.points = static_cast<std::uint32_t>(std::clamp(count, 3.0, static_cast<double>(kMaxShapePoints)));
                if (static_cast<double>(shape.points) != count) {
                    approximated("a star or polygon has 3.." + std::to_string(kMaxShapePoints) + " points", where + "/pt");
                }
                shape.outerRadius = param(readProp(member(item, "or"), time, where + "/or"), 0, 100.0, 1.0, 0.0, 0.0, kCoord, where + "/or");
                shape.innerRadius = param(readProp(member(item, "ir"), time, where + "/ir"), 0, 50.0, 1.0, 0.0, 0.0, kCoord, where + "/ir");
                layer.transform.rotation = param(readProp(member(item, "r"), time, where + "/r"), 0, 0.0, 1.0, 0.0, -1e6, 1e6, where + "/r");
                for (const char* key : {"os", "is"}) {
                    const RawProp roundness = readProp(member(item, key), time, where + "/" + key);
                    if (roundness.present && (roundness.animated || at(roundness.value, 0, 0.0) != 0.0)) {
                        approximated("star and polygon roundness is not imported", where + "/" + key);
                        break;
                    }
                }
                if (round) {
                    approximated("rounded corners apply to rectangles only", round->where);
                }
            }
        }
        if (fill) {
            shape.fill = fillOf(*fill, ox, oy, time);
        } else {
            shape.fill.kind = PaintKind::None;
        }
        if (stroke) {
            shape.stroke = strokeOf(*stroke, time);
        }
        if (trim) {
            const Json& t = *trim->item;
            shape.trimStart = param(readProp(member(t, "s"), time, trim->where + "/s"), 0, 0.0, 1.0, 0.0, 0.0, 100.0, trim->where + "/s");
            shape.trimEnd = param(readProp(member(t, "e"), time, trim->where + "/e"), 0, 100.0, 1.0, 0.0, 0.0, 100.0, trim->where + "/e");
            shape.trimOffset = param(readProp(member(t, "o"), time, trim->where + "/o"), 0, 0.0, 1.0, 0.0, -1e6, 1e6, trim->where + "/o");
            if (numberOr(t, "m", 1.0) == 2.0) {
                approximated("trim paths applied individually trims each path on its own", trim->where);
            }
        }
        return layer;
    }

    // ---- Text -------------------------------------------------------------------------------------------------------

    static std::int32_t weightOf(std::string style) {
        std::transform(style.begin(), style.end(), style.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        std::erase_if(style, [](char c) { return c == ' ' || c == '-' || c == '_'; });
        const std::pair<const char*, std::int32_t> kWeights[] = {{"extrabold", 800}, {"ultrabold", 800}, {"semibold", 600}, {"demibold", 600},
                                                                {"extralight", 200}, {"ultralight", 200}, {"thin", 100},     {"hairline", 100},
                                                                {"light", 300},      {"medium", 500},     {"black", 900},    {"heavy", 900},
                                                                {"bold", 700}};
        for (const auto& [name, weight] : kWeights) {
            if (style.find(name) != std::string::npos) {
                return weight;
            }
        }
        return 400;
    }

    /// Fills a text layer from a Lottie text layer; the content offset its anchor needs.
    Content convertText(const Json& data, GraphicLayer& layer, const TimeMap& time, const std::string& where) {
        Content content;
        const Json* d = member(data, "d");
        const Json* keys = d ? array(*d, "k") : nullptr;
        if (!keys || keys->empty()) {
            dropped("a text layer without text", where);
            return content;
        }
        if (keys->size() > 1) {
            approximated("text that changes over time keeps its first text", where + "/d");
        }
        const Json* doc = member((*keys)[0], "s");
        if (!doc || !doc->is_object()) {
            dropped("a text layer without text", where);
            return content;
        }
        std::string text = stringOf(*doc, "t");
        std::string normalised;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '\r') {
                normalised += '\n';
                if (i + 1 < text.size() && text[i + 1] == '\n') {
                    ++i;
                }
            } else if (c == '\x03') {
                normalised += '\n';
            } else {
                normalised += c;
            }
        }
        if (normalised.size() > kMaxGraphicTextBytes) {
            approximated("text longer than 16 KiB is cut", where);
        }
        TextRun run;
        run.text = bounded(std::move(normalised), kMaxGraphicTextBytes);
        TextStyle& style = run.style;
        style.fontSize = std::clamp(numberOr(*doc, "s", 72.0), 1.0, 2000.0);
        double ascent = kDefaultAscent;
        const std::string fontName = stringOf(*doc, "f");
        if (const auto f = m_fonts.find(fontName); f != m_fonts.end()) {
            style.fontFamily = bounded(stringOf(*f->second, "fFamily"), kMaxFontBytes);
            style.fontStyle = bounded(stringOf(*f->second, "fStyle"), kMaxFontBytes);
            if (const auto a = number(member(*f->second, "ascent")); a && *a > 0.0 && *a < 200.0) {
                ascent = *a / 100.0;
            }
        } else {
            style.fontFamily = bounded(fontName, kMaxFontBytes);
        }
        style.fontWeight = weightOf(style.fontStyle);
        const std::vector<double> fc = numbers(member(*doc, "fc"));
        for (std::size_t i = 0; i < 3; ++i) {
            style.fill.rgba[i] = std::clamp(at(fc, i, 1.0), 0.0, 1.0);
        }
        style.fill.rgba[3] = fc.size() >= 4 ? std::clamp(fc[3], 0.0, 1.0) : 1.0;
        style.tracking = std::clamp(numberOr(*doc, "tr", 0.0), -1000.0, 10000.0);
        style.leading = std::clamp(numberOr(*doc, "lh", 0.0), 0.0, 10000.0);
        style.baselineShift = std::clamp(-numberOr(*doc, "ls", 0.0), -10000.0, 10000.0);
        if (numberOr(*doc, "sw", 0.0) > 0.0 && member(*doc, "sc")) {
            approximated("a text stroke is not imported", where + "/d");
        }
        const int justify = intOr(*doc, "j", 0);
        layer.text.align = justify == 1 ? TextAlign::Right : justify == 2 ? TextAlign::Center : justify >= 3 && justify <= 6 ? TextAlign::Justify : TextAlign::Left;
        const double alignFactor = layer.text.align == TextAlign::Center ? 0.5 : layer.text.align == TextAlign::Right ? 1.0 : 0.0;
        layer.text.runs = {std::move(run)};
        const std::vector<double> box = numbers(member(*doc, "sz"));
        if (box.size() >= 2 && box[0] > 0.0) {
            // Paragraph text: the box's top-left is at ps; Wabcut's text block hangs from its top-left, top-centre or
            // top-right (by alignment).
            const std::vector<double> ps = numbers(member(*doc, "ps"));
            layer.text.boxWidth = std::clamp(box[0], 0.0, kMaxGraphicCoordinate);
            layer.text.boxHeight = std::clamp(box[1], 0.0, kMaxGraphicCoordinate);
            content.x = std::clamp(at(ps, 0, 0.0) + alignFactor * layer.text.boxWidth, -kMaxGraphicCoordinate, kMaxGraphicCoordinate);
            content.y = std::clamp(at(ps, 1, 0.0), -kMaxGraphicCoordinate, kMaxGraphicCoordinate);
        } else {
            // Point text: the layer's origin is on the first line's baseline.
            content.y = -ascent * layer.text.runs[0].style.fontSize;
            approximated("point text is placed by its font's ascent; its layout follows Wabcut's text engine", where);
        }
        if (const Json* path = member(data, "p"); path && path->is_object() && member(*path, "m")) {
            approximated("text on a path is drawn as plain text", where + "/p");
        }
        if (const Json* animators = array(data, "a")) {
            for (std::size_t i = 0; i < animators->size(); ++i) {
                if (layer.text.animators.size() >= kMaxTextAnimators) {
                    dropped("more than " + std::to_string(kMaxTextAnimators) + " text animators", where + "/a");
                    break;
                }
                if (auto a = textAnimator((*animators)[i], time, where + "/a/" + std::to_string(i))) {
                    layer.text.animators.push_back(std::move(*a));
                }
            }
        }
        return content;
    }

    std::optional<TextAnimator> textAnimator(const Json& a, const TimeMap& time, const std::string& where) {
        const Json* s = member(a, "s");
        const Json* props = member(a, "a");
        if (!s || !s->is_object() || !props || !props->is_object()) {
            dropped("a text animator without a range selector", where);
            return std::nullopt;
        }
        if (numberOr(*s, "t", 0.0) != 0.0) {
            dropped("an expression selector is not imported", where);
            return std::nullopt;
        }
        if (numberOr(*s, "r", 1.0) == 2.0) {
            dropped("a range selector counted in index units is not imported", where);
            return std::nullopt;
        }
        TextAnimator out;
        out.name = bounded(stringOf(a, "nm"), kMaxNameBytes);
        const int basedOn = intOr(*s, "b", 1);
        out.unit = basedOn == 3 ? TextUnit::Word : basedOn == 4 ? TextUnit::Line : TextUnit::Character;
        if (basedOn == 2) {
            approximated("characters excluding spaces are counted with spaces", where + "/s/b");
        }
        const int shape = intOr(*s, "sh", 1);
        out.shape = shape == 1 ? TextSelectorShape::Square : TextSelectorShape::Ramp;
        if (shape > 2) {
            approximated("this selector shape is imported as a ramp", where + "/s/sh");
        }
        out.start = param(readProp(member(*s, "s"), time, where + "/s/s"), 0, 0.0, 1.0, 0.0, -100.0, 200.0, where + "/s/s");
        out.end = param(readProp(member(*s, "e"), time, where + "/s/e"), 0, 100.0, 1.0, 0.0, -100.0, 200.0, where + "/s/e");
        out.offset = param(readProp(member(*s, "o"), time, where + "/s/o"), 0, 0.0, 1.0, 0.0, -200.0, 200.0, where + "/s/o");
        for (const char* key : {"a", "rn", "xe", "ne"}) {
            const RawProp p = readProp(member(*s, key), time, where + "/s/" + key);
            const double neutral = std::string(key) == "a" ? 100.0 : 0.0;
            if (p.present && (p.animated || at(p.value, 0, neutral) != neutral)) {
                approximated("the selector's amount, ease and randomness are not imported", where + "/s");
                break;
            }
        }
        const auto first = [&](const char* key, std::size_t dim, double fallback) {
            const RawProp p = readProp(member(*props, key), time, where + "/a/" + key);
            return p.present ? at(firstValue(p, where + "/a/" + key, "animator value"), dim, fallback) : fallback;
        };
        out.opacity = std::clamp(first("o", 0, 100.0), 0.0, 100.0);
        out.offsetX = std::clamp(first("p", 0, 0.0), -kMaxGraphicCoordinate, kMaxGraphicCoordinate);
        out.offsetY = std::clamp(first("p", 1, 0.0), -kMaxGraphicCoordinate, kMaxGraphicCoordinate);
        out.scale = std::clamp(first("s", 0, 100.0), -100000.0, 100000.0);
        out.rotation = std::clamp(first("r", 0, 0.0), -1e6, 1e6);
        for (auto it = props->begin(); it != props->end(); ++it) {
            const std::string& key = it.key();
            if (key != "o" && key != "p" && key != "s" && key != "r") {
                approximated("text animator property \"" + bounded(key, 16) + "\" is not imported", where + "/a");
                break;
            }
        }
        return out;
    }

    // ---- Masks and effects ------------------------------------------------------------------------------------------

    std::vector<Mask> masksOf(const Json& layer, const TimeMap& time, Content content, const std::string& where) {
        std::vector<Mask> out;
        const Json* list = array(layer, "masksProperties");
        if (!list) {
            return out;
        }
        const double w = m_width;
        const double h = m_height;
        for (std::size_t i = 0; i < list->size(); ++i) {
            const Json& m = (*list)[i];
            const std::string here = where + "/masksProperties/" + std::to_string(i);
            const std::string mode = stringOf(m, "mode");
            if (mode == "n") {
                continue; // a mask that does nothing
            }
            if (out.size() >= kMaxMasks - 1) { // one place stays free for a precomp's clip
                dropped("more than " + std::to_string(kMaxMasks - 1) + " masks on a layer", here);
                break;
            }
            Mask mask;
            mask.id = static_cast<std::uint32_t>(out.size() + 1);
            mask.name = bounded(stringOf(m, "nm"), kMaxMaskName);
            mask.shape = MaskShape::Path;
            mask.inverted = flag(m, "inv");
            mask.mode = mode == "s" ? MaskMode::Subtract : mode == "i" ? MaskMode::Intersect : MaskMode::Add;
            if (mode != "a" && mode != "s" && mode != "i" && !mode.empty()) {
                approximated("mask mode \"" + bounded(mode, 8) + "\" is imported as add", here);
            }
            const auto keys = pathKeys(member(m, "pt"), time, 1.0 / w, 1.0 / h, content.x, content.y, here + "/pt");
            if (!keys || keys->second.front().vertices.size() < 3 || keys->second.front().vertices.size() > kMaxMaskVertices) {
                dropped("a mask needs 3.." + std::to_string(kMaxMaskVertices) + " vertices", here);
                continue;
            }
            mask.path = keys->second;
            if (!keys->first) {
                approximated("an open mask path is closed", here);
            }
            if (const RawProp o = readProp(member(m, "o"), time, here + "/o"); o.present) {
                mask.set("opacity", param(o, 0, 100.0, 0.01, 0.0, 0.0, 1.0, here + "/o"));
            }
            if (const RawProp x = readProp(member(m, "x"), time, here + "/x"); x.present) {
                mask.set("expansion", param(x, 0, 0.0, 1.0 / h, 0.0, -1.0, 1.0, here + "/x"));
            }
            if (auto problem = maskProblem(mask)) {
                dropped("a mask that cannot be imported: " + *problem, here);
                continue;
            }
            out.push_back(std::move(mask));
        }
        return out;
    }

    void effects(const Json& layer, GraphicLayer& out, const std::string& where) {
        const Json* list = array(layer, "ef");
        if (!list) {
            return;
        }
        for (std::size_t i = 0; i < list->size() && i < 1000; ++i) {
            const Json& e = (*list)[i];
            const std::string here = where + "/ef/" + std::to_string(i);
            const std::string name = bounded(stringOf(e, "nm"), 64);
            if (numberOr(e, "ty", 0.0) == 25.0 && !out.shadow.enabled) {
                // Drop Shadow: colour, opacity (0..255), direction (degrees clockwise from up), distance, softness.
                const Json* values = array(e, "ef");
                const auto value = [&](std::size_t index, double fallback) {
                    if (!values || index >= values->size()) {
                        return fallback;
                    }
                    const Json* v = member((*values)[index], "v");
                    const std::vector<double> k = v && v->is_object() ? numbers(member(*v, "k")) : std::vector<double>{};
                    return at(k, 0, fallback);
                };
                DropShadow& s = out.shadow;
                s.enabled = true;
                if (values && !values->empty()) {
                    const Json* v = member((*values)[0], "v");
                    const std::vector<double> c = v && v->is_object() ? numbers(member(*v, "k")) : std::vector<double>{};
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        s.color.rgba[ch] = std::clamp(at(c, ch, 0.0), 0.0, 1.0);
                    }
                }
                s.opacity = std::clamp(value(1, 127.5) / 255.0, 0.0, 1.0);
                s.angle = std::clamp(value(2, 135.0) - 90.0, -1e6, 1e6);
                s.distance = std::clamp(value(3, 5.0), 0.0, 10000.0);
                s.softness = std::clamp(value(4, 0.0), 0.0, 10000.0);
                note(ReportOutcome::Approximated, ReportCategory::Effect,
                     "effect '" + name + "' becomes the layer's drop shadow (animated values keep their first)", here);
                continue;
            }
            note(ReportOutcome::Dropped, ReportCategory::Effect, "effect '" + name + "' is not imported", here);
        }
    }

    static BlendMode blendOf(int bm, bool& known) {
        known = true;
        switch (bm) {
        case 0:
            return BlendMode::Normal;
        case 1:
            return BlendMode::Multiply;
        case 2:
            return BlendMode::Screen;
        case 3:
            return BlendMode::Overlay;
        case 4:
            return BlendMode::Darken;
        case 5:
            return BlendMode::Lighten;
        case 10:
            return BlendMode::Difference;
        case 16:
            return BlendMode::Add;
        default:
            known = false;
            return BlendMode::Normal;
        }
    }

    // ---- Layers -----------------------------------------------------------------------------------------------------

    /// The layers of a composition (`layers`, first on top) as Wabcut layers bottom to top, at `depth`. `time` maps the
    /// composition's frames to root frames; `stack` holds the precomps being converted (cycles).
    std::vector<GraphicLayer> convertComp(const Json& layers, const TimeMap& time, std::size_t depth,
                                          std::vector<std::string>& stack, const std::string& where) {
        std::vector<GraphicLayer> out;
        if (!layers.is_array()) {
            return out;
        }
        if (layers.size() > kMaxLottieLayersPerComp) {
            throw Refused{"a composition has more than " + std::to_string(kMaxLottieLayersPerComp) + " layers"};
        }
        std::map<double, const Json*> byIndex;
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (const auto ind = number(member(layers[i], "ind"))) {
                byIndex.emplace(*ind, &layers[i]);
            }
        }
        for (std::size_t r = layers.size(); r-- > 0;) {
            if (m_layerCount >= kMaxGraphicLayers) {
                takeLayer(where); // reports once
                break;
            }
            work();
            const Json& l = layers[r];
            const std::string here = where + "/" + std::to_string(r);
            const std::size_t notesBefore = m_layerNotes;
            auto layer = convertLayer(l, byIndex, time, depth, stack, here);
            if (layer) {
                if (m_layerNotes == notesBefore) {
                    const std::string own = bounded(stringOf(l, "nm"), kMaxNameBytes); // not a parent wrapper's
                    const std::string name = own.empty() ? std::string() : " '" + own + "'";
                    note(ReportOutcome::Carried, ReportCategory::Graphic, std::string(kindName(l)) + " layer" + name, here);
                }
                out.push_back(std::move(*layer));
            }
        }
        return out;
    }

    static const char* kindName(const Json& l) {
        switch (intOr(l, "ty", -1)) {
        case 0:
            return "precomp";
        case 1:
            return "solid";
        case 4:
            return "shape";
        case 5:
            return "text";
        default:
            return "a";
        }
    }

    std::optional<GraphicLayer> convertLayer(const Json& l, const std::map<double, const Json*>& byIndex, const TimeMap& time,
                                             std::size_t depth, std::vector<std::string>& stack, const std::string& where) {
        if (!l.is_object()) {
            dropped("a layer that is not an object", where);
            return std::nullopt;
        }
        const int ty = intOr(l, "ty", -1);
        const std::string name = bounded(stringOf(l, "nm"), kMaxNameBytes);
        const std::string label = name.empty() ? std::string("a layer") : "layer '" + name + "'";
        if (flag(l, "td")) {
            dropped(label + " is a track matte: mattes are not imported", where);
            return std::nullopt;
        }
        if (ty == 3) {
            return std::nullopt; // a null: only its children use it, through their parent groups
        }
        if (ty == 2) {
            dropped(label + " is an image: images in Lottie files are not imported", where, ReportCategory::Media);
            return std::nullopt;
        }
        if (ty != 0 && ty != 1 && ty != 4 && ty != 5) {
            dropped(label + " has a layer type Wabcut does not import (" + std::to_string(ty) + ")", where);
            return std::nullopt;
        }
        // Times: in and out in composition time; keys in layer time (composition time - st), stretched by sr.
        const double st = std::clamp(numberOr(l, "st", 0.0), -kMaxFrames, kMaxFrames);
        const double sr = numberOr(l, "sr", 1.0);
        if (!(sr > 0.0) || sr > 1000.0) {
            dropped(label + " has a bad time stretch", where);
            return std::nullopt;
        }
        const TimeMap local{time(st), time.scale};
        const double ip = std::clamp(numberOr(l, "ip", 0.0), -kMaxFrames, kMaxFrames);
        const double op = std::clamp(numberOr(l, "op", ip + 1e6), -kMaxFrames, kMaxFrames);
        const auto inAt = ticks(time(ip));
        const auto outAt = ticks(time(op));
        if (!inAt || !outAt || *outAt <= Ticks{} || *outAt <= *inAt || *inAt >= m_duration) {
            dropped(label + " is never visible in the animation's time", where);
            return std::nullopt;
        }

        // The parents' transforms wrap the layer (without their opacity: parenting does not pass it on).
        std::vector<const Json*> parents;
        std::set<double> seen;
        if (const auto ind = number(member(l, "ind"))) {
            seen.insert(*ind);
        }
        for (auto parent = number(member(l, "parent")); parent;) {
            const auto it = byIndex.find(*parent);
            if (it == byIndex.end() || !seen.insert(*parent).second) {
                approximated(label + " has a missing or circular parent: it is placed without it", where + "/parent");
                break;
            }
            parents.push_back(it->second);
            parent = number(member(*it->second, "parent"));
        }
        const std::size_t room = kMaxGraphicDepth - depth; // groups that fit above the layer
        if (parents.size() > room) {
            approximated(label + " has parents nested deeper than the graphic allows: the outermost are left out", where + "/parent");
            parents.resize(room);
        }
        const std::size_t layerDepth = depth + parents.size();

        if (!takeLayer(where)) {
            return std::nullopt;
        }
        GraphicLayer layer;
        layer.id = nextId();
        layer.name = name;
        layer.visible = !flag(l, "hd");
        layer.in = std::max(*inAt, Ticks{});
        if (*outAt < m_duration) {
            layer.out = *outAt;
        }
        bool knownBlend = true;
        layer.blend = blendOf(intOr(l, "bm", 0), knownBlend);
        if (!knownBlend) {
            approximated(label + "'s blend mode is imported as normal", where + "/bm");
        }
        if (flag(l, "ddd")) {
            approximated(label + " is a 3D layer: it is imported flat", where + "/ddd");
        }
        if (flag(l, "ao")) {
            approximated(label + "'s auto-orient is not imported", where + "/ao");
        }
        if (member(l, "tt")) {
            approximated(label + " is drawn without its track matte", where + "/tt");
        }
        if (member(l, "tm")) {
            approximated(label + "'s time remapping is not imported", where + "/tm");
        }

        Content content;
        switch (ty) {
        case 0: { // precomp: a group of the asset's layers, clipped to the precomp's size
            layer.kind = LayerKind::Group;
            const std::string ref = stringOf(l, "refId");
            const auto asset = m_assets.find(ref);
            const Json* assetLayers = asset != m_assets.end() ? array(*asset->second.first, "layers") : nullptr;
            if (!assetLayers) {
                dropped(label + " refers to a missing composition", where + "/refId");
                --m_layerCount;
                return std::nullopt;
            }
            if (std::find(stack.begin(), stack.end(), ref) != stack.end()) {
                dropped(label + " contains itself", where + "/refId");
                --m_layerCount;
                return std::nullopt;
            }
            if (layerDepth >= kMaxGraphicDepth) {
                dropped(label + ": precomps nested deeper than " + std::to_string(kMaxGraphicDepth) + " levels", where);
                --m_layerCount;
                return std::nullopt;
            }
            // The precomp's frame c shows at layer time c * sr, so at composition time st + c * sr.
            const TimeMap inner{time(st), time.scale * sr};
            stack.push_back(ref);
            layer.children = convertComp(*assetLayers, inner, layerDepth + 1, stack,
                                         "/assets/" + std::to_string(asset->second.second) + "/layers");
            stack.pop_back();
            break;
        }
        case 1: { // solid: a rectangle from (0, 0) to (sw, sh)
            layer.kind = LayerKind::Shape;
            const double sw = std::clamp(numberOr(l, "sw", 0.0), 0.0, kMaxGraphicCoordinate);
            const double sh = std::clamp(numberOr(l, "sh", 0.0), 0.0, kMaxGraphicCoordinate);
            layer.shape.kind = ShapeKind::Rectangle;
            layer.shape.width = ParamValue(sw);
            layer.shape.height = ParamValue(sh);
            const auto color = hexColor(stringOf(l, "sc"));
            layer.shape.fill.color.rgba = color.value_or(std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
            content = {sw / 2.0, sh / 2.0};
            break;
        }
        case 4: { // shape: a group of its shapes
            layer.kind = LayerKind::Group;
            if (layerDepth >= kMaxGraphicDepth) {
                dropped(label + ": shapes nested deeper than " + std::to_string(kMaxGraphicDepth) + " levels", where);
                --m_layerCount;
                return std::nullopt;
            }
            if (const Json* shapes = array(l, "shapes")) {
                layer.children = convertShapes(*shapes, {}, local, layerDepth + 1, where + "/shapes");
            }
            break;
        }
        case 5: { // text
            layer.kind = LayerKind::Text;
            if (const Json* t = member(l, "t"); t && t->is_object()) {
                content = convertText(*t, layer, local, where + "/t");
            }
            if (layer.text.runs.empty()) {
                --m_layerCount;
                return std::nullopt;
            }
            break;
        }
        default:
            break;
        }
        layer.transform = transformOf(member(l, "ks"), local, content, true, where + "/ks");
        layer.masks = masksOf(l, local, content, where);
        if (ty == 0) {
            // The clip to the precomp's w x h, intersected with the layer's own masks.
            const double w = std::clamp(numberOr(l, "w", m_width), 1.0, kMaxGraphicCoordinate);
            const double h = std::clamp(numberOr(l, "h", m_height), 1.0, kMaxGraphicCoordinate);
            Mask clip;
            clip.id = static_cast<std::uint32_t>(layer.masks.size() + 1);
            clip.name = "Precomp bounds";
            clip.shape = MaskShape::Rectangle;
            clip.mode = MaskMode::Intersect;
            const double sx = std::min(w / m_width, 20.0);
            const double sy = std::min(h / m_height, 20.0);
            clip.set("center.x", ParamValue(std::min(sx / 2.0, 10.0)));
            clip.set("center.y", ParamValue(std::min(sy / 2.0, 10.0)));
            clip.set("size.x", ParamValue(sx));
            clip.set("size.y", ParamValue(sy));
            layer.masks.push_back(std::move(clip));
        }
        effects(l, layer, where);

        // Wrap it in its parents' transforms, the outermost parent outside.
        GraphicLayer result = std::move(layer);
        for (std::size_t i = 0; i < parents.size(); ++i) {
            const Json& p = *parents[i];
            if (!takeLayer(where)) {
                return result; // without the rest of its parents: the layer count is full
            }
            GraphicLayer wrapper;
            wrapper.id = nextId();
            wrapper.kind = LayerKind::Group;
            wrapper.name = bounded("Parent: " + stringOf(p, "nm"), kMaxNameBytes);
            const double pst = std::clamp(numberOr(p, "st", 0.0), -kMaxFrames, kMaxFrames);
            wrapper.transform = transformOf(member(p, "ks"), TimeMap{time(pst), time.scale}, {}, false,
                                            where + "/parent");
            wrapper.children.push_back(std::move(result));
            result = std::move(wrapper);
        }
        return result;
    }

    const Json& m_root;
    std::string m_file;
    ImportReport m_report;
    std::size_t m_suppressed = 0;
    std::size_t m_layerNotes = 0;
    double m_fr = 30.0;
    double m_ip = 0.0;
    Rate m_rate;
    Ticks m_duration{};
    std::int32_t m_width = 1920;
    std::int32_t m_height = 1080;
    std::map<std::string, std::pair<const Json*, std::size_t>> m_assets;
    std::map<std::string, const Json*> m_fonts;
    std::size_t m_layerCount = 0;
    bool m_fullReported = false;
    std::uint32_t m_lastId = 0;
    std::size_t m_totalKeys = 0;
    std::size_t m_totalVertices = 0;
    std::size_t m_work = 0;
};

} // namespace

std::expected<LottieImport, Error> importLottie(std::string_view json, std::string_view fileName) {
    if (json.size() > kMaxLottieBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "the Lottie file is larger than 16 MiB"});
    }
    if (!io::jsonNestingWithin(json)) {
        return std::unexpected(Error{ErrorCode::ParseError, "the Lottie file nests too deep"});
    }
    const Json root = Json::parse(json, nullptr, false);
    if (root.is_discarded()) {
        return std::unexpected(Error{ErrorCode::ParseError, "the Lottie file is not valid JSON (UTF-8)"});
    }
    LottieImport result;
    try {
        result = Importer(root, std::string(fileName)).run();
    } catch (const Refused& r) {
        return std::unexpected(Error{ErrorCode::ParseError, "Lottie: " + r.message});
    } catch (const Json::exception& e) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string("Lottie: ") + e.what()});
    }
    if (const auto problem = graphicProblem(result.graphic)) {
        return std::unexpected(Error{ErrorCode::ParseError, "internal: the imported graphic " + *problem});
    }
    return result;
}

} // namespace wabcut::interchange
