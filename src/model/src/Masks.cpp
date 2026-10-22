#include <wabcut/model/Masks.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>

namespace wabcut {

namespace {

constexpr double kBig = 1e12;

// Sorted by key.
constexpr std::array<MaskParamInfo, 17> kParams{{
    {"center.x", 0.5, -10.0, 10.0},
    {"center.y", 0.5, -10.0, 10.0},
    {"expansion", 0.0, -1.0, 1.0},
    {"feather", 0.0, 0.0, 1.0},
    {"opacity", 1.0, 0.0, 1.0},
    {"rotation", 0.0, -36000.0, 36000.0},
    {"roundness", 0.0, 0.0, 1.0},
    {"size.x", 0.5, 0.0, 20.0},
    {"size.y", 0.5, 0.0, 20.0},
    {"transform.anchorX", 0.5, -10.0, 10.0},
    {"transform.anchorY", 0.5, -10.0, 10.0},
    {"transform.rotation", 0.0, -36000.0, 36000.0},
    {"transform.scaleX", 1.0, -100.0, 100.0},
    {"transform.scaleY", 1.0, -100.0, 100.0},
    {"transform.skew", 0.0, -85.0, 85.0},
    {"transform.x", 0.0, -10.0, 10.0},
    {"transform.y", 0.0, -10.0, 10.0},
}};

/// Bezier circle constant: four cubic arcs approximate a circle within 0.03 %.
constexpr double kKappa = 0.5522847498307936;

double radians(double degrees) { return degrees * std::numbers::pi / 180.0; }

bool finite(double v) { return std::isfinite(v); }

bool finite(const MaskVertex& v) {
    return finite(v.x) && finite(v.y) && finite(v.inX) && finite(v.inY) && finite(v.outX) && finite(v.outY);
}

/// Progress through a segment from A to B shaped by A's leaving side and B's arriving side: a cubic Hermite from 0 to
/// 1 with slope 1 (linear) or 0 (eased) at each end. Monotonic.
double eased(double u, Interpolation leaving, Interpolation arriving) {
    const auto flat = [](Interpolation i, bool leavingSide) {
        switch (i) {
        case Interpolation::EaseInOut:
        case Interpolation::Bezier:
            return true;
        case Interpolation::EaseOut:
            return leavingSide;
        case Interpolation::EaseIn:
            return !leavingSide;
        default:
            return false;
        }
    };
    const double s0 = flat(leaving, true) ? 0.0 : 1.0;
    const double s1 = flat(arriving, false) ? 0.0 : 1.0;
    const double u2 = u * u;
    const double u3 = u2 * u;
    return (u3 - 2.0 * u2 + u) * s0 + (-2.0 * u3 + 3.0 * u2) + (u3 - u2) * s1;
}

MaskVertex lerp(const MaskVertex& a, const MaskVertex& b, double t) {
    const auto l = [t](double x, double y) { return x + (y - x) * t; };
    return {l(a.x, b.x), l(a.y, b.y), l(a.inX, b.inX), l(a.inY, b.inY), l(a.outX, b.outX), l(a.outY, b.outY)};
}

/// A vertex in aspect space (x in frame heights) rotated by `angle` radians about the origin, then moved to `centre`.
MaskVertex placed(double x, double y, double inX, double inY, double outX, double outY, double angle, double cx, double cy) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const auto rx = [&](double px, double py) { return px * c - py * s; };
    const auto ry = [&](double px, double py) { return px * s + py * c; };
    return {cx + rx(x, y), cy + ry(x, y), rx(inX, inY), ry(inX, inY), rx(outX, outY), ry(outX, outY)};
}

/// Aspect space (x in frame heights) to normalised coordinates.
void toNormalised(std::vector<MaskVertex>& vertices, double aspect) {
    for (MaskVertex& v : vertices) {
        v.x /= aspect;
        v.inX /= aspect;
        v.outX /= aspect;
    }
}

} // namespace

// ---- Names -------------------------------------------------------------------------------------------------------

std::string_view toString(MaskShape shape) {
    switch (shape) {
    case MaskShape::Ellipse:
        return "ellipse";
    case MaskShape::Rectangle:
        return "rectangle";
    case MaskShape::Path:
        return "path";
    case MaskShape::Matte:
        return "matte";
    }
    return "ellipse";
}

std::optional<MaskShape> maskShapeFromString(std::string_view text) {
    for (const MaskShape s : {MaskShape::Ellipse, MaskShape::Rectangle, MaskShape::Path, MaskShape::Matte}) {
        if (toString(s) == text) {
            return s;
        }
    }
    return std::nullopt;
}

std::string_view toString(MaskMode mode) {
    switch (mode) {
    case MaskMode::Add:
        return "add";
    case MaskMode::Subtract:
        return "subtract";
    case MaskMode::Intersect:
        return "intersect";
    }
    return "add";
}

std::optional<MaskMode> maskModeFromString(std::string_view text) {
    for (const MaskMode m : {MaskMode::Add, MaskMode::Subtract, MaskMode::Intersect}) {
        if (toString(m) == text) {
            return m;
        }
    }
    return std::nullopt;
}

std::string_view toString(RedactionStyle style) {
    switch (style) {
    case RedactionStyle::Blur:
        return "blur";
    case RedactionStyle::Mosaic:
        return "mosaic";
    case RedactionStyle::Solid:
        return "solid";
    }
    return "blur";
}

std::optional<RedactionStyle> redactionStyleFromString(std::string_view text) {
    for (const RedactionStyle s : {RedactionStyle::Blur, RedactionStyle::Mosaic, RedactionStyle::Solid}) {
        if (toString(s) == text) {
            return s;
        }
    }
    return std::nullopt;
}

std::string_view toString(RedactionAudio audio) {
    switch (audio) {
    case RedactionAudio::None:
        return "none";
    case RedactionAudio::Mute:
        return "mute";
    case RedactionAudio::Bleep:
        return "bleep";
    }
    return "none";
}

std::optional<RedactionAudio> redactionAudioFromString(std::string_view text) {
    for (const RedactionAudio a : {RedactionAudio::None, RedactionAudio::Mute, RedactionAudio::Bleep}) {
        if (toString(a) == text) {
            return a;
        }
    }
    return std::nullopt;
}

// ---- Parameters --------------------------------------------------------------------------------------------------

std::span<const MaskParamInfo> maskParams() { return kParams; }

const MaskParamInfo* maskParam(std::string_view key) {
    const auto it = std::ranges::find(kParams, key, &MaskParamInfo::key);
    return it == kParams.end() ? nullptr : &*it;
}

const ParamValue* Mask::find(std::string_view key) const {
    const auto it = std::lower_bound(params.begin(), params.end(), key,
                                     [](const MaskParam& p, std::string_view k) { return p.key < k; });
    return it != params.end() && it->key == key ? &it->value : nullptr;
}

void Mask::set(std::string key, ParamValue value) {
    const auto it = std::lower_bound(params.begin(), params.end(), key,
                                     [](const MaskParam& p, const std::string& k) { return p.key < k; });
    if (it != params.end() && it->key == key) {
        it->value = std::move(value);
    } else {
        params.insert(it, MaskParam{std::move(key), std::move(value)});
    }
}

double maskValue(const Mask& mask, std::string_view key, Ticks curveTime) {
    if (const ParamValue* v = mask.find(key)) {
        return v->valueAt(curveTime);
    }
    const MaskParamInfo* info = maskParam(key);
    return info ? info->defaultValue : 0.0;
}

// ---- Geometry ----------------------------------------------------------------------------------------------------

std::vector<MaskVertex> maskPathAt(const Mask& mask, Ticks curveTime) {
    const std::vector<MaskPathKey>& keys = mask.path;
    if (keys.empty()) {
        return {};
    }
    if (curveTime <= keys.front().time) {
        return keys.front().vertices;
    }
    if (curveTime >= keys.back().time) {
        return keys.back().vertices;
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), curveTime,
                                       [](Ticks t, const MaskPathKey& k) { return t < k.time; });
    const MaskPathKey& b = *next;
    const MaskPathKey& a = *(next - 1);
    if (a.interpolation == Interpolation::Hold || a.vertices.size() != b.vertices.size()) {
        return a.vertices;
    }
    const double u = static_cast<double>((curveTime - a.time).value) / static_cast<double>((b.time - a.time).value);
    const double t = eased(std::clamp(u, 0.0, 1.0), a.interpolation, b.interpolation);
    std::vector<MaskVertex> out(a.vertices.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = lerp(a.vertices[i], b.vertices[i], t);
    }
    return out;
}

std::vector<MaskVertex> maskShapeAt(const Mask& mask, Ticks curveTime, double aspect) {
    aspect = aspect > 0.0 && std::isfinite(aspect) ? aspect : 1.0;
    if (mask.shape == MaskShape::Path) {
        std::vector<MaskVertex> path = maskPathAt(mask, curveTime);
        return path.size() >= 3 ? path : std::vector<MaskVertex>{};
    }
    const auto v = [&](std::string_view key) { return maskValue(mask, key, curveTime); };
    const double cx = v("center.x") * aspect;
    const double cy = v("center.y");
    const double w = std::abs(v("size.x")) * aspect;
    const double h = std::abs(v("size.y"));
    const double angle = radians(v("rotation"));
    if (!(w > 0.0) || !(h > 0.0)) {
        return {};
    }
    std::vector<MaskVertex> out;
    if (mask.shape == MaskShape::Ellipse) {
        const double rx = w / 2.0;
        const double ry = h / 2.0;
        const double kx = kKappa * rx;
        const double ky = kKappa * ry;
        // Clockwise on screen (y down): right, bottom, left, top.
        out.push_back(placed(rx, 0.0, 0.0, -ky, 0.0, ky, angle, cx, cy));
        out.push_back(placed(0.0, ry, kx, 0.0, -kx, 0.0, angle, cx, cy));
        out.push_back(placed(-rx, 0.0, 0.0, ky, 0.0, -ky, angle, cx, cy));
        out.push_back(placed(0.0, -ry, -kx, 0.0, kx, 0.0, angle, cx, cy));
    } else {
        const double hx = w / 2.0;
        const double hy = h / 2.0;
        const double r = std::clamp(v("roundness"), 0.0, 1.0) * std::min(hx, hy);
        if (r <= 1e-9) {
            out.push_back(placed(-hx, -hy, 0, 0, 0, 0, angle, cx, cy));
            out.push_back(placed(hx, -hy, 0, 0, 0, 0, angle, cx, cy));
            out.push_back(placed(hx, hy, 0, 0, 0, 0, angle, cx, cy));
            out.push_back(placed(-hx, hy, 0, 0, 0, 0, angle, cx, cy));
        } else {
            const double k = kKappa * r;
            // Each corner: the arc's two ends; straight edges between them (tangents zero on the edge side).
            out.push_back(placed(-hx + r, -hy, -k, 0, 0, 0, angle, cx, cy)); // top edge start
            out.push_back(placed(hx - r, -hy, 0, 0, k, 0, angle, cx, cy));   // top edge end -> top-right arc
            out.push_back(placed(hx, -hy + r, 0, -k, 0, 0, angle, cx, cy));
            out.push_back(placed(hx, hy - r, 0, 0, 0, k, angle, cx, cy));
            out.push_back(placed(hx - r, hy, k, 0, 0, 0, angle, cx, cy));
            out.push_back(placed(-hx + r, hy, 0, 0, -k, 0, angle, cx, cy));
            out.push_back(placed(-hx, hy - r, 0, k, 0, 0, angle, cx, cy));
            out.push_back(placed(-hx, -hy + r, 0, 0, 0, -k, angle, cx, cy));
        }
    }
    toNormalised(out, aspect);
    return out;
}

MaskAffine maskTransformAt(const Mask& mask, Ticks curveTime, double aspect) {
    aspect = aspect > 0.0 && std::isfinite(aspect) ? aspect : 1.0;
    const auto v = [&](std::string_view key) { return maskValue(mask, key, curveTime); };
    const double rot = radians(v("transform.rotation"));
    const double skew = std::tan(radians(std::clamp(v("transform.skew"), -85.0, 85.0)));
    const double sx = v("transform.scaleX");
    const double sy = v("transform.scaleY");
    // L = R(rot) * Shear(skew) * S(sx, sy), in aspect space.
    const double c = std::cos(rot);
    const double s = std::sin(rot);
    const double m00 = c * sx;
    const double m01 = (c * skew - s) * sy;
    const double m10 = s * sx;
    const double m11 = (s * skew + c) * sy;
    const double ax = v("transform.anchorX") * aspect;
    const double ay = v("transform.anchorY");
    const double tx = v("transform.x") * aspect;
    const double ty = v("transform.y");
    const double ox = ax + tx - (m00 * ax + m01 * ay);
    const double oy = ay + ty - (m10 * ax + m11 * ay);
    return {m00, m01 / aspect, ox / aspect, m10 * aspect, m11, oy};
}

std::vector<MaskVertex> maskOutlineAt(const Mask& mask, Ticks curveTime, double aspect) {
    std::vector<MaskVertex> out = maskShapeAt(mask, curveTime, aspect);
    const MaskAffine m = maskTransformAt(mask, curveTime, aspect);
    if (m == MaskAffine{}) {
        return out;
    }
    for (MaskVertex& v : out) {
        const MaskVertex p = v;
        v.x = m.a * p.x + m.b * p.y + m.c;
        v.y = m.d * p.x + m.e * p.y + m.f;
        v.inX = m.a * p.inX + m.b * p.inY;
        v.inY = m.d * p.inX + m.e * p.inY;
        v.outX = m.a * p.outX + m.b * p.outY;
        v.outY = m.d * p.outX + m.e * p.outY;
    }
    return out;
}

// ---- Bounds ------------------------------------------------------------------------------------------------------

std::optional<std::string> maskProblem(const Mask& mask) {
    if (static_cast<int>(mask.shape) > static_cast<int>(MaskShape::Matte) ||
        static_cast<int>(mask.mode) > static_cast<int>(MaskMode::Intersect)) {
        return "an unknown mask shape or mode";
    }
    if (mask.name.size() > kMaxMaskName) {
        return "a mask name longer than " + std::to_string(kMaxMaskName) + " bytes";
    }
    for (std::size_t i = 0; i < mask.params.size(); ++i) {
        const MaskParam& p = mask.params[i];
        if (p.key.empty() || (i > 0 && !(mask.params[i - 1].key < p.key))) {
            return "mask parameters must have unique, sorted, non-empty keys";
        }
        const MaskParamInfo* info = maskParam(p.key);
        const double lo = info ? info->minValue : -kBig;
        const double hi = info ? info->maxValue : kBig;
        if (!p.value.isValid(lo, hi)) {
            return "mask parameter \"" + p.key + "\" is out of range or a malformed curve";
        }
    }
    if (mask.shape == MaskShape::Path && mask.path.empty()) {
        return "a path mask needs at least one key";
    }
    if ((mask.shape == MaskShape::Matte) != mask.matte.has_value()) {
        return mask.matte ? "only a matte mask has a matte track" : "a matte mask needs its matte track";
    }
    if (mask.matte) {
        const MaskMatte& m = *mask.matte;
        if (m.track.empty() || m.track.size() > kMaxMatteTrack || m.model.size() > kMaxMatteTrack) {
            return "a matte track needs an identity of 1.." + std::to_string(kMaxMatteTrack) + " bytes";
        }
        if (m.media.isValid() == m.sequence.isValid() || m.stream < 0 || m.stream > 64 || m.source.start.value < 0 ||
            m.source.duration.value < 0) {
            return "a matte track needs its media (or its nested sequence), stream and source range";
        }
        // The bounding box is what a frame without a matte shows (fail closed): always stored with the mask.
        for (const char* key : {"center.x", "center.y", "size.x", "size.y"}) {
            if (!mask.find(key)) {
                return "a matte mask keeps its bounding box (center and size)";
            }
        }
    }
    if (mask.path.size() > kMaxMaskPathKeys) {
        return "more than " + std::to_string(kMaxMaskPathKeys) + " path keys";
    }
    for (std::size_t i = 0; i < mask.path.size(); ++i) {
        const MaskPathKey& k = mask.path[i];
        if (k.vertices.size() < 3 || k.vertices.size() > kMaxMaskVertices) {
            return "a path key needs 3.." + std::to_string(kMaxMaskVertices) + " vertices";
        }
        if (k.vertices.size() != mask.path.front().vertices.size()) {
            return "every path key needs the same number of vertices";
        }
        if (i > 0 && !(mask.path[i - 1].time < k.time)) {
            return "path keys must be at increasing times";
        }
        if (static_cast<int>(k.interpolation) > static_cast<int>(Interpolation::EaseInOut)) {
            return "an unknown path key interpolation";
        }
        for (const MaskVertex& v : k.vertices) {
            if (!finite(v)) {
                return "a path vertex is not finite";
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> masksProblem(std::span<const Mask> masks) {
    if (masks.size() > kMaxMasks) {
        return "more than " + std::to_string(kMaxMasks) + " masks";
    }
    std::set<std::uint32_t> ids;
    for (const Mask& m : masks) {
        if (m.id == 0 || !ids.insert(m.id).second) {
            return "masks need unique non-zero ids";
        }
        if (auto problem = maskProblem(m)) {
            return "mask " + std::to_string(m.id) + ": " + *problem;
        }
    }
    return std::nullopt;
}

// ---- Redactions --------------------------------------------------------------------------------------------------

bool Redaction::appliesTo(ClipId clip) const {
    return enabled && !std::binary_search(exceptions.begin(), exceptions.end(), clip);
}

std::optional<std::string> redactionProblem(const Redaction& r) {
    if (!r.media.isValid()) {
        return "a redaction needs a media item";
    }
    if (r.name.size() > kMaxMaskName) {
        return "a redaction name longer than " + std::to_string(kMaxMaskName) + " bytes";
    }
    if (r.source.start.value < 0 || r.source.duration.value <= 0) {
        return "a redaction needs a source range of positive length";
    }
    // The end must be representable: evaluation and the audio path compute start + duration (Codex wave-4 P2-11).
    if (!checkedAdd(r.source.start, r.source.duration)) {
        return "a redaction's source range ends past the largest time";
    }
    if (static_cast<int>(r.style) > static_cast<int>(RedactionStyle::Solid) ||
        static_cast<int>(r.audio) > static_cast<int>(RedactionAudio::Bleep)) {
        return "an unknown redaction style or audio treatment";
    }
    if (!std::isfinite(r.strength) || r.strength <= 0.0 || r.strength > 1.0) {
        return "a redaction strength must be in (0, 1]";
    }
    for (const double c : {r.colorR, r.colorG, r.colorB}) {
        if (!std::isfinite(c) || c < 0.0 || c > 1.0) {
            return "a redaction colour must be in [0, 1]";
        }
    }
    if (r.audioRanges.size() > kMaxRedactionRanges || r.exceptions.size() > kMaxRedactionRanges) {
        return "more than " + std::to_string(kMaxRedactionRanges) + " audio ranges or exceptions";
    }
    for (const TimeRange& range : r.audioRanges) {
        if (range.start.value < 0 || range.duration.value <= 0) {
            return "a redaction audio range needs a positive length";
        }
        if (!checkedAdd(range.start, range.duration)) {
            return "a redaction audio range ends past the largest time";
        }
    }
    for (std::size_t i = 0; i < r.exceptions.size(); ++i) {
        if (!r.exceptions[i].isValid() || (i > 0 && !(r.exceptions[i - 1] < r.exceptions[i]))) {
            return "redaction exceptions must be sorted, unique clip ids";
        }
    }
    if (auto problem = maskProblem(r.region)) {
        return "its region: " + *problem;
    }
    return std::nullopt;
}

} // namespace wabcut
