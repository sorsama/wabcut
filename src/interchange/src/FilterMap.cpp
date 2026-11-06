// Colour grades and audio EQ / dynamics in FCP7 XML (FilterMap.h).

#include "FilterMap.h"

#include "TextUtil.h"

#include <wabcut/model/Plural.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <numbers>

namespace wabcut::interchange::detail {

namespace {

/// Lower-case letters and digits only: "EQ Band 1 Gain" -> "eqband1gain", "Faded Film" -> "fadedfilm".
std::string norm(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
        }
    }
    return out;
}

std::string number(double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%.3g", v);
    return text;
}

const EffectDescriptor* descriptorOf(std::string_view id) { return EffectRegistry::builtin().find(id); }

/// A filter's effect name without Premiere's match-name prefix ("AE.ADBE Lumetri" -> "Lumetri").
std::string effectKey(const FcpFilter& f) {
    std::string_view id = f.id;
    for (const std::string_view prefix : {std::string_view("AE.ADBE "), std::string_view("ADBE ")}) {
        if (id.size() > prefix.size() && iequals(id.substr(0, prefix.size()), prefix)) {
            id.remove_prefix(prefix.size());
        }
    }
    return norm(id);
}

bool named(const FcpFilter& f, std::initializer_list<std::string_view> names) {
    const std::string n = norm(f.name);
    const std::string id = effectKey(f);
    for (const std::string_view candidate : names) {
        if (n == candidate || id == candidate) {
            return true;
        }
    }
    return false;
}

std::string paramKey(const FcpFilterParam& p) { return norm(p.name.empty() ? p.id : p.name); }

/// Builds one Wabcut effect: values checked against the descriptor's ranges (clamped and noted when outside).
class Mapped {
public:
    Mapped(std::string_view effect, std::string label) {
        m_out.effect.effect = std::string(effect);
        m_out.label = std::move(label);
        m_descriptor = descriptorOf(effect);
    }

    /// `p` converted by `convert` (constants and every key) into parameter `key`.
    bool set(std::string_view key, const FcpFilterParam& p, const std::function<double(double)>& convert = {}) {
        if (!p.value) {
            return false;
        }
        ParamValue v = *p.value;
        if (convert) {
            if (const Curve* source = v.curve()) {
                Curve curve = *source;
                for (Keyframe& k : curve.keys) {
                    k.value = convert(k.value);
                }
                v = ParamValue(std::move(curve));
            } else {
                v = ParamValue(convert(v.constant()));
            }
        }
        return setValue(key, std::move(v), p.name.empty() ? p.id : p.name);
    }
    bool setValue(std::string_view key, ParamValue v, std::string_view what) {
        const EffectParamInfo* info = m_descriptor ? m_descriptor->param(key) : nullptr;
        if (!info) {
            return false;
        }
        bool clamped = false;
        auto clamp = [&](double x) {
            if (!std::isfinite(x)) {
                clamped = true;
                return info->defaultValue;
            }
            const double c = std::clamp(x, info->minValue, info->maxValue);
            clamped = clamped || c != x;
            return c;
        };
        if (const Curve* source = v.curve()) {
            Curve curve = *source;
            for (Keyframe& k : curve.keys) {
                k.value = clamp(k.value);
            }
            v = ParamValue(std::move(curve));
        } else {
            v = ParamValue(clamp(v.constant()));
        }
        if (!v.isValid(info->minValue, info->maxValue)) {
            m_out.dropped.push_back(std::string(what) + " (malformed keyframes)");
            return false;
        }
        if (clamped) {
            m_out.approximated.push_back(std::string(what) + " was outside Wabcut's range: clamped");
        }
        m_out.effect.set(std::string(key), std::move(v));
        return true;
    }
    void setConstant(std::string_view key, double v, std::string_view what) { setValue(key, ParamValue(v), what); }
    void approximated(std::string note) { m_out.approximated.push_back(std::move(note)); }
    void dropped(std::string note) { m_out.dropped.push_back(std::move(note)); }
    void data(EffectData d) { m_out.effect.setData(std::move(d)); }
    const EffectDescriptor* descriptor() const { return m_descriptor; }
    MappedFilter take() { return std::move(m_out); }

private:
    MappedFilter m_out;
    const EffectDescriptor* m_descriptor = nullptr;
};

/// The constant of a parameter (its value at the first key when keyframed, noted).
double constantOf(const FcpFilterParam& p, double fallback, Mapped& m) {
    if (!p.value) {
        return fallback;
    }
    if (p.value->isCurve()) {
        m.approximated(std::string(p.name.empty() ? p.id : p.name) + " keyframes: its first value is used");
    }
    return p.value->constant();
}

// --- Colour -----------------------------------------------------------------------------------------------------

/// Channel hues of the RGB primaries, for balance wheels.
constexpr double kChannelHue[3] = {0.0, 120.0, 240.0};
constexpr const char* kChannels[3] = {"red", "green", "blue"};

double cosDeg(double deg) { return std::cos(deg * std::numbers::pi / 180.0); }

/// A colour (0..255 or 0..1) as hue in degrees and amount 0..100 (its saturation).
std::pair<double, double> hueAmount(std::array<double, 3> rgb) {
    const bool bytes = std::any_of(rgb.begin(), rgb.end(), [](double c) { return c > 1.0; });
    for (double& c : rgb) {
        c = std::clamp(bytes ? c / 255.0 : c, 0.0, 1.0);
    }
    const double mx = std::max({rgb[0], rgb[1], rgb[2]});
    const double mn = std::min({rgb[0], rgb[1], rgb[2]});
    const double d = mx - mn;
    double hue = 0.0;
    if (d > 0) {
        if (mx == rgb[0]) {
            hue = 60.0 * std::fmod((rgb[1] - rgb[2]) / d, 6.0);
        } else if (mx == rgb[1]) {
            hue = 60.0 * ((rgb[2] - rgb[0]) / d + 2.0);
        } else {
            hue = 60.0 * ((rgb[0] - rgb[1]) / d + 4.0);
        }
    }
    if (hue < 0) {
        hue += 360.0;
    }
    return {hue, mx > 0 ? d / mx * 100.0 : 0.0};
}

/// Levels (input black / gray / white, output black / white, 0..255 and a gamma) as an ASC CDL on the grade's wheels.
void levelsAsCdl(Mapped& m, double inBlack, double inGray, double inWhite, double outBlack, double outWhite) {
    if (inBlack == 0.0 && inWhite == 255.0 && outBlack == 0.0 && outWhite == 255.0 && inGray == 1.0) {
        return;
    }
    const double span = std::max(inWhite - inBlack, 1.0);
    const double slope = (outWhite - outBlack) / span;
    const double offset = (outBlack - inBlack * slope) / 255.0;
    const double power = inGray > 0.0 ? 1.0 / inGray : 1.0;
    for (const char* c : kChannels) {
        m.setConstant(std::string("wheels.gain.") + c, slope, "levels");
        m.setConstant(std::string("wheels.offset.") + c, offset, "levels");
        m.setConstant(std::string("wheels.gamma.") + c, power, "levels");
    }
    m.approximated("input " + number(inBlack) + " / " + number(inGray) + " / " + number(inWhite) + " and output " +
                   number(outBlack) + " / " + number(outWhite) + " levels became a CDL: slope " + number(slope) +
                   ", offset " + number(offset) + ", power " + number(power));
}

/// A balance wheel (magnitude 0..100 at an angle in degrees) on one CDL control: lift and offset add, gain multiplies,
/// gamma (power) divides.
void balanceWheel(Mapped& m, std::string_view control, double magnitude, double angle, std::string_view what) {
    if (magnitude == 0.0) {
        return;
    }
    const double strength = std::clamp(magnitude, 0.0, 100.0) / 100.0;
    for (int c = 0; c < 3; ++c) {
        const double push = strength * cosDeg(angle - kChannelHue[c]);
        const std::string key = "wheels." + std::string(control) + "." + kChannels[c];
        const double value = control == "lift" ? 0.1 * push : control == "gamma" ? 1.0 / (1.0 + 0.25 * push) : 1.0 + 0.25 * push;
        m.setConstant(key, value, what);
    }
    m.approximated(std::string(what) + " (" + number(magnitude) + " at " + number(angle) + " degrees) became the " +
                   std::string(control) + " wheel");
}

/// "x,y x,y" / "x y;x y" points (0..1, or 0..255) sampled into at most 32 curve points.
std::optional<std::vector<EffectPoint>> curvePoints(std::string_view text) {
    std::vector<double> numbers;
    std::size_t at = 0;
    while (at < text.size() && numbers.size() < 4096) {
        while (at < text.size() && !(std::isdigit(static_cast<unsigned char>(text[at])) || text[at] == '-' || text[at] == '.')) {
            ++at;
        }
        if (at >= text.size()) {
            break;
        }
        double v = 0;
        const auto [end, ec] = std::from_chars(text.data() + at, text.data() + text.size(), v);
        if (ec != std::errc{}) {
            ++at;
            continue;
        }
        numbers.push_back(v);
        at = static_cast<std::size_t>(end - text.data());
    }
    if (numbers.size() < 4 || numbers.size() % 2 != 0) {
        return std::nullopt;
    }
    const bool bytes = std::any_of(numbers.begin(), numbers.end(), [](double v) { return v > 1.0; });
    std::vector<EffectPoint> points;
    for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
        const double x = bytes ? numbers[i] / 255.0 : numbers[i];
        const double y = bytes ? numbers[i + 1] / 255.0 : numbers[i + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return std::nullopt;
        }
        points.push_back({std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)});
    }
    std::stable_sort(points.begin(), points.end(), [](const EffectPoint& a, const EffectPoint& b) { return a.x < b.x; });
    points.erase(std::unique(points.begin(), points.end(), [](const EffectPoint& a, const EffectPoint& b) { return a.x == b.x; }),
                 points.end());
    if (points.size() < 2) {
        return std::nullopt;
    }
    if (points.size() > 32) { // sampled: 17 points, linear between the given ones
        std::vector<EffectPoint> sampled;
        for (int i = 0; i <= 16; ++i) {
            const double x = points.front().x + (points.back().x - points.front().x) * i / 16.0;
            const auto next = std::lower_bound(points.begin(), points.end(), x, [](const EffectPoint& p, double v) { return p.x < v; });
            double y = next == points.end() ? points.back().y : next->y;
            if (next != points.begin() && next != points.end() && next->x != x) {
                const auto prev = next - 1;
                y = prev->y + (next->y - prev->y) * (x - prev->x) / (next->x - prev->x);
            }
            sampled.push_back({x, y});
        }
        points = std::move(sampled);
    }
    return points;
}

// --- Premiere's Lumetri Color as Premiere writes it (PF) -------------------------------------------------------------

/// Base64 (RFC 4648, whitespace skipped) to bytes; nullopt when malformed or longer than `maxBytes`.
std::optional<std::string> fromBase64(std::string_view in, std::size_t maxBytes) {
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    bool padding = false;
    for (const char c : in) {
        int v = -1;
        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+' || c == '-') {
            v = 62;
        } else if (c == '/' || c == '_') {
            v = 63;
        } else if (c == '=') {
            padding = true;
            continue;
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        } else {
            return std::nullopt;
        }
        if (padding) {
            return std::nullopt; // data after the padding
        }
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            if (out.size() > maxBytes) {
                return std::nullopt;
            }
        }
    }
    return out;
}

/// What Premiere's Lumetri state block (parameter 1, "Blob": base64 of a <Lumetri> text with one <shader> per stage
/// and its settings as <Key>"value"</Key>) says beyond the sliders: LUT files, and the stages FCP7 XML has no
/// parameters for (curves, colour wheels, HSL secondary) when they are set.
struct LumetriBlob {
    bool readable = false;
    std::vector<std::string> luts;
    std::vector<std::string> edited;
};

/// A Lumetri setting's number: "D0", "N100", "2.4" (the letter is Premiere's own tag).
std::optional<double> blobNumber(std::string_view v) {
    while (!v.empty() && std::isalpha(static_cast<unsigned char>(v.front()))) {
        v.remove_prefix(1);
    }
    double x = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec != std::errc{} || end != v.data() + v.size() || !std::isfinite(x)) {
        return std::nullopt;
    }
    return x;
}

/// A curve setting "cubic,value:0@0;1@1": its "a@b" points as pairs (a = the curve's value, b = its position).
std::vector<std::pair<double, double>> blobCurve(std::string_view v) {
    std::vector<std::pair<double, double>> points;
    if (const std::size_t colon = v.find(':'); colon != std::string_view::npos) {
        v.remove_prefix(colon + 1);
    }
    while (!v.empty() && points.size() < 256) {
        const std::size_t end = std::min(v.find(';'), v.size());
        const std::string_view item = v.substr(0, end);
        const std::size_t at = item.find('@');
        double a = 0;
        double b = 0;
        if (at != std::string_view::npos && std::from_chars(item.data(), item.data() + at, a).ec == std::errc{} &&
            std::from_chars(item.data() + at + 1, item.data() + item.size(), b).ec == std::errc{}) {
            points.emplace_back(a, b);
        }
        v.remove_prefix(std::min(end + 1, v.size()));
    }
    return points;
}

LumetriBlob inspectLumetriBlob(std::string_view base64) {
    LumetriBlob out;
    const auto decoded = fromBase64(base64, 4u << 20);
    if (!decoded) {
        return out;
    }
    const std::string_view text = *decoded;
    if (text.find("<Lumetri>") == std::string_view::npos) {
        return out;
    }
    out.readable = true;
    auto addEdited = [&](std::string what) {
        if (std::find(out.edited.begin(), out.edited.end(), what) == out.edited.end()) {
            out.edited.push_back(std::move(what));
        }
    };
    std::size_t at = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        const std::size_t begin = text.find("<shader>", at);
        if (begin == std::string_view::npos) {
            break;
        }
        const std::size_t end = std::min(text.find("</shader>", begin), text.size());
        const std::string_view shader = text.substr(begin, end - begin);
        at = end;
        // Its name, then every <Key>"value"</Key> of its <parameters> (and <shadermask>).
        std::string_view name;
        if (const std::size_t n = shader.find("<name>\""); n != std::string_view::npos) {
            const std::size_t start = n + 7;
            name = shader.substr(start, shader.find('"', start) - start);
        }
        std::size_t p = shader.find("<parameters>");
        while (p != std::string_view::npos) {
            const std::size_t open = shader.find('<', p + 1);
            if (open == std::string_view::npos || shader.compare(open, 2, "</") == 0) {
                break;
            }
            const std::size_t close = shader.find('>', open);
            if (close == std::string_view::npos) {
                break;
            }
            const std::string_view key = shader.substr(open + 1, close - open - 1);
            std::string_view value;
            if (close + 1 < shader.size() && shader[close + 1] == '"') {
                const std::size_t q = shader.find('"', close + 2);
                if (q == std::string_view::npos) {
                    break;
                }
                value = shader.substr(close + 2, q - close - 2);
            }
            p = shader.find('>', close + 1); // past the closing tag
            if (key == "LUT" && !value.empty()) {
                const std::size_t slash = value.find_last_of("/\\");
                out.luts.emplace_back(slash == std::string_view::npos ? value : value.substr(slash + 1));
            } else if (name == "ToneCurves" && key.starts_with("ToneCurves")) {
                const auto points = blobCurve(value);
                const bool identity = points.size() == 2 && points[0] == std::pair(0.0, 0.0) && points[1] == std::pair(1.0, 1.0);
                if (!identity && !points.empty()) {
                    addEdited("RGB curves");
                }
            } else if (name == "ColorCurves" && key.ends_with("Curve")) {
                const auto points = blobCurve(value);
                if (points.size() > 2 || (points.size() == 2 && points[0].first != points[1].first)) {
                    addEdited("hue / saturation curves");
                }
            } else if ((name == "Wheels" || name == "SecondaryLayer") && key != "DiffWhite") {
                const bool wheelOrAdjust = key.starts_with("Shadows") || key.starts_with("Midtones") ||
                                           key.starts_with("Highlights") || key.starts_with("Specular") || key == "Temp" ||
                                           key == "Tint" || key == "Contrast" || key == "Sharpness" || key == "Denoise" ||
                                           key == "Blur";
                const auto number = blobNumber(value);
                if (wheelOrAdjust && number && *number != 0.0) {
                    addEdited(name == "Wheels" ? "colour wheels" : "HSL secondary");
                } else if (name == "SecondaryLayer" && key == "Saturation" && number && *number != 100.0) {
                    addEdited("HSL secondary");
                }
            }
        }
    }
    return out;
}

/// Premiere's Lumetri parameter ids: the id, its name in Effect Controls (normalised; empty for the unnamed section
/// switches and state blocks), and the Wabcut key (empty when it is not a value Wabcut sets).
struct LumetriId {
    std::string_view id;
    std::string_view name;
    std::string_view key;
};
constexpr LumetriId kLumetriIds[] = {
    {"1", "blob", ""},
    {"3", "", "basic.enabled"},
    {"4", "", ""},
    {"5", "inputlut", ""},
    {"7", "temperature", "basic.temperature"},
    {"8", "tint", "basic.tint"},
    {"11", "exposure", "basic.exposure"},
    {"12", "contrast", "basic.contrast"},
    {"13", "highlights", "basic.highlights"},
    {"14", "shadows", "basic.shadows"},
    {"15", "whites", "basic.whites"},
    {"16", "blacks", "basic.blacks"},
    {"20", "saturation", "basic.saturation"},
    {"23", "", "creative.enabled"},
    {"24", "", ""},
    {"25", "look", ""},
    {"26", "intensity", "creative.lookIntensity"},
    {"28", "fadedfilm", "creative.fadedFilm"},
    {"29", "sharpen", ""},
    {"30", "vibrance", "creative.vibrance"},
    {"31", "saturation", "creative.saturation"},
    {"32", "", ""},
    {"33", "tintbalance", "creative.tintBalance"},
    {"37", "", "curves.enabled"},
    {"39", "", ""},
    {"42", "huevssat", ""},
    {"46", "", "wheels.enabled"},
    {"47", "", ""},
    {"50", "", "vignette.enabled"},
    {"51", "amount", "vignette.amount"},
    {"52", "midpoint", "vignette.midpoint"},
    {"53", "roundness", "vignette.roundness"},
    {"54", "feather", "vignette.feather"},
};

/// The Premiere id row of a parameter: its id is one of Premiere's and its name is empty or the same (a hand-written
/// file that numbers its parameters its own way keeps matching by name).
const LumetriId* lumetriIdOf(const FcpFilterParam& p) {
    const std::string n = norm(p.name);
    for (const LumetriId& row : kLumetriIds) {
        if (p.id == row.id && (n.empty() || n == row.name)) {
            return &row;
        }
    }
    return nullptr;
}

std::optional<MappedFilter> lumetri(const FcpFilter& f) {
    Mapped m("wabcut.color.grade", "Lumetri Color");
    enum class Section { Basic, Creative, Vignette } section = Section::Basic;
    bool basicSaturation = false;
    std::vector<std::string> unmapped;
    std::vector<std::string> lutIndex; // Premiere's Look / Input LUT menus: only a place in its list
    LumetriBlob blob;
    bool blobSeen = false;
    for (const FcpFilterParam& p : f.params) {
        if (m.descriptor() && m.descriptor()->param(p.id) && p.value) { // Wabcut's own ids ("basic.exposure")
            m.set(p.id, p);
            section = p.id.starts_with("creative.") ? Section::Creative : p.id.starts_with("vignette.") ? Section::Vignette : section;
            basicSaturation = basicSaturation || p.id == "basic.saturation";
            continue;
        }
        if (const LumetriId* row = lumetriIdOf(p)) { // Premiere's numeric ids
            const std::string what = p.name.empty() || norm(p.name).empty() ? "Lumetri parameter " + p.id : p.name;
            if (row->id == "1") {
                blobSeen = true;
                blob = inspectLumetriBlob(p.text);
            } else if (row->id == "5" || row->id == "25") {
                if (p.value && p.value->constant() != 0.0) {
                    lutIndex.push_back(std::string(row->id == "5" ? "Input LUT" : "Look") + " no. " + number(p.value->constant()) +
                                       " of Premiere's list");
                } else if (!p.text.empty() && !p.value) {
                    lutIndex.push_back(std::string(row->id == "5" ? "Input LUT" : "Look") + " '" + p.text + "'");
                }
            } else if (row->id == "29") {
                if (p.value && !(p.value->isConstant() && p.value->constant() == 0.0)) {
                    m.dropped("Sharpen " + number(p.value->constant()));
                }
            } else if (!row->key.empty() && p.value) {
                const std::string_view key = row->key;
                if (key.ends_with(".enabled")) {
                    if (!(p.value->isConstant() && p.value->constant() != 0.0)) { // a section switched off
                        m.setValue(key, *p.value, what);
                    }
                } else {
                    m.set(key, p);
                }
                section = key.starts_with("creative.") ? Section::Creative : key.starts_with("vignette.") ? Section::Vignette : section;
                basicSaturation = basicSaturation || key == "basic.saturation";
            }
            continue; // the other ids are Premiere's state blocks (curves, wheels): read through the Blob
        }
        const std::string k = paramKey(p);
        if (k == "intensity" || k == "lookintensity" || k == "fadedfilm" || k == "sharpen" || k == "look" || k == "creativelook" ||
            k == "tintbalance" || k == "shadowtint" || k == "highlighttint") {
            section = Section::Creative;
        } else if (k == "amount" || k == "midpoint" || k == "roundness" || k == "feather" || k.starts_with("vignette")) {
            section = Section::Vignette;
        }
        static const std::map<std::string, std::string, std::less<>> kBasic = {
            {"temperature", "basic.temperature"}, {"whitebalancetemperature", "basic.temperature"}, {"tint", "basic.tint"},
            {"exposure", "basic.exposure"},       {"contrast", "basic.contrast"},       {"highlights", "basic.highlights"},
            {"shadows", "basic.shadows"},         {"whites", "basic.whites"},           {"blacks", "basic.blacks"},
            {"intensity", "creative.lookIntensity"}, {"lookintensity", "creative.lookIntensity"},
            {"fadedfilm", "creative.fadedFilm"},  {"tintbalance", "creative.tintBalance"},
            {"amount", "vignette.amount"},        {"vignetteamount", "vignette.amount"}, {"midpoint", "vignette.midpoint"},
            {"vignettemidpoint", "vignette.midpoint"}, {"roundness", "vignette.roundness"}, {"vignetteroundness", "vignette.roundness"},
            {"feather", "vignette.feather"},      {"vignettefeather", "vignette.feather"},
        };
        if (k == "saturation") {
            const bool basic = section == Section::Basic && !basicSaturation;
            m.set(basic ? "basic.saturation" : "creative.saturation", p);
            basicSaturation = basicSaturation || basic;
        } else if (k == "vibrance") {
            m.set(section == Section::Basic ? "basic.vibrance" : "creative.vibrance", p);
        } else if (const auto it = kBasic.find(k); it != kBasic.end() && p.value) {
            m.set(it->second, p);
        } else if ((k == "shadowtint" || k == "highlighttint") && p.color) {
            const auto [hue, amount] = hueAmount(*p.color);
            const std::string stage = k == "shadowtint" ? "creative.shadowTint." : "creative.highlightTint.";
            m.setConstant(stage + "hue", hue, p.name);
            m.setConstant(stage + "amount", amount, p.name);
            m.approximated(p.name + " colour became hue " + number(hue) + " and amount " + number(amount));
        } else if (k == "look" || k == "creativelook" || k == "inputlut" || k == "lut") {
            if (!p.text.empty()) {
                m.dropped(p.name + " LUT '" + p.text + "' (FCP7 XML has no LUT path: apply it again in Color)");
            }
        } else if (k == "sharpen") {
            if (p.value && !(p.value->isConstant() && p.value->constant() == 0.0)) {
                m.dropped("Sharpen " + number(p.value->constant()));
            }
        } else if (!k.empty()) {
            unmapped.push_back(p.name.empty() ? p.id : p.name);
        }
    }
    // LUTs: by file name from the Blob when it names them, else by their place in Premiere's menus.
    if (!blob.luts.empty()) {
        for (const std::string& lut : blob.luts) {
            m.dropped("LUT '" + lut + "' (FCP7 XML has no LUT path: apply it again in Color)");
        }
    } else {
        for (const std::string& lut : lutIndex) {
            m.dropped(lut + " (FCP7 XML has no LUT file: apply it again in Color)");
        }
    }
    if (blobSeen && !blob.readable) {
        m.dropped("Premiere's Lumetri state block could not be read: curves, colour wheels and HSL secondary set there "
                  "were not imported");
    } else if (!blob.edited.empty()) {
        std::string list;
        for (const std::string& e : blob.edited) {
            list += (list.empty() ? "" : ", ") + e;
        }
        m.dropped("Lumetri " + list + " (kept only in Premiere's own state block, which FCP7 XML readers do not share)");
    }
    if (!unmapped.empty()) {
        std::string list;
        for (std::size_t i = 0; i < unmapped.size() && i < 8; ++i) {
            list += (i ? ", " : "") + unmapped[i];
        }
        m.dropped(counted(static_cast<std::int64_t>(unmapped.size()), "parameter") + " Wabcut cannot place (" + list +
                  (unmapped.size() > 8 ? ", ..." : "") + ")");
    }
    return m.take();
}

std::optional<MappedFilter> colorCorrector(const FcpFilter& f, bool threeWay) {
    Mapped m("wabcut.color.grade", threeWay ? "Three-Way Color Corrector" : "Fast Color Corrector");
    double levels[5] = {0.0, 1.0, 255.0, 0.0, 255.0}; // in black, gray, white, out black, white
    struct Wheel {
        double magnitude = 0.0;
        double angle = 0.0;
    };
    Wheel wheels[3]; // shadows, midtones, highlights (Fast Color Corrector: its one balance on the midtones)
    std::vector<std::string> unmapped;
    for (const FcpFilterParam& p : f.params) {
        const std::string k = paramKey(p);
        const auto has = [&](std::string_view part) { return k.find(part) != std::string::npos; };
        const int wheel = (has("shadow") || (has("black") && has("balance")))      ? 0
                          : (has("highlight") || (has("white") && has("balance"))) ? 2
                                                                                   : 1;
        if (k == "saturation" || k == "mastersaturation") {
            m.set("basic.saturation", p);
        } else if (has("inputblack")) {
            levels[0] = constantOf(p, 0.0, m);
        } else if (has("inputgray") || has("inputgrey")) {
            levels[1] = constantOf(p, 1.0, m);
        } else if (has("inputwhite")) {
            levels[2] = constantOf(p, 255.0, m);
        } else if (has("outputblack")) {
            levels[3] = constantOf(p, 0.0, m);
        } else if (has("outputwhite")) {
            levels[4] = constantOf(p, 255.0, m);
        } else if (has("balancemagnitude")) {
            wheels[wheel].magnitude = constantOf(p, 0.0, m);
        } else if (has("balanceangle")) {
            wheels[wheel].angle = constantOf(p, 0.0, m);
        } else if (has("balance") && p.point) { // FCP7's colour correctors: a balance point on the wheel
            const double x = (*p.point)[0];
            const double y = (*p.point)[1];
            wheels[wheel].magnitude = std::min(100.0, std::hypot(x, y) * 100.0);
            wheels[wheel].angle = std::atan2(y, x) * 180.0 / std::numbers::pi;
        } else if (has("balancegain") || (has("lightness") && !has("saturation"))) {
            // A wheel's gain beside its magnitude: carried by the magnitude alone.
        } else if (!k.empty() && (p.value || p.point || p.color)) {
            const bool neutral = p.value && p.value->isConstant() && p.value->constant() == 0.0;
            if (!neutral) {
                unmapped.push_back(p.name.empty() ? p.id : p.name);
            }
        }
    }
    levelsAsCdl(m, levels[0], levels[1], levels[2], levels[3], levels[4]);
    static constexpr const char* kControl[3] = {"lift", "gamma", "gain"};
    static constexpr const char* kWheelName[3] = {"shadow balance", "midtone balance", "highlight balance"};
    for (int i = 0; i < 3; ++i) {
        balanceWheel(m, kControl[i], wheels[i].magnitude, wheels[i].angle, threeWay ? kWheelName[i] : "the balance");
    }
    for (const std::string& u : unmapped) {
        m.dropped(u);
    }
    return m.take();
}

std::optional<MappedFilter> rgbCurves(const FcpFilter& f) {
    Mapped m("wabcut.color.grade", "RGB Curves");
    bool any = false;
    for (const FcpFilterParam& p : f.params) {
        const std::string k = paramKey(p);
        const char* key = k == "master" || k == "rgb" || k == "composite" || k == "luma" ? "curves.master"
                          : k == "red"                                                 ? "curves.red"
                          : k == "green"                                               ? "curves.green"
                          : k == "blue"                                                ? "curves.blue"
                                                                                       : nullptr;
        if (!key) {
            continue;
        }
        if (const auto points = curvePoints(p.text)) {
            m.data(EffectData{key, *points});
            any = true;
        } else if (!p.text.empty() || p.value) {
            m.dropped(p.name + " curve (its points are not written as a list Wabcut reads)");
        }
    }
    if (any) {
        m.approximated("the curves were sampled into curve points");
    }
    return m.take();
}

/// FCP7's 3 Band Equalizer and Premiere's legacy EQ -> wabcut.audio.eq3.
std::optional<MappedFilter> threeBandEq(const FcpFilter& f) {
    Mapped m("wabcut.audio.eq3", f.name.empty() ? std::string("3 Band Equalizer") : f.name);
    for (const FcpFilterParam& p : f.params) {
        std::string k = paramKey(p);
        for (const std::string_view mid : {"mid1", "midrange", "mids"}) {
            if (k.starts_with(mid)) {
                k = "mid" + k.substr(mid.size());
            }
        }
        if (k.starts_with("bass")) {
            k = "low" + k.substr(4);
        }
        if (k.starts_with("treble")) {
            k = "high" + k.substr(6);
        }
        static const std::map<std::string, std::string, std::less<>> kKeys = {
            {"lowfrequency", "lowFrequency"}, {"lowfreq", "lowFrequency"},   {"lowgain", "lowGain"},
            {"midfrequency", "midFrequency"}, {"midfreq", "midFrequency"},   {"midgain", "midGain"},
            {"midq", "midQ"},                 {"highfrequency", "highFrequency"}, {"highfreq", "highFrequency"},
            {"highgain", "highGain"},
        };
        if (const auto it = kKeys.find(k); it != kKeys.end()) {
            m.set(it->second, p);
        } else if (p.value && !(p.value->isConstant() && p.value->constant() == 0.0) && k.find("gain") != std::string::npos) {
            m.dropped(p.name + " " + number(p.value->constant()) + " dB (Wabcut's EQ has three bands)");
        }
    }
    return m.take();
}

/// FCP7's single-band Parametric Equalizer (Frequency, Q, Gain) -> band 1 of wabcut.audio.parametricEq.
std::optional<MappedFilter> singleBandEq(const FcpFilter& f) {
    Mapped m("wabcut.audio.parametricEq", f.name.empty() ? std::string("Parametric Equalizer") : f.name);
    for (const FcpFilterParam& p : f.params) {
        const std::string k = paramKey(p);
        if (k == "frequency") {
            m.set("band1Frequency", p);
        } else if (k == "q") {
            m.set("band1Q", p);
        } else if (k == "gain") {
            m.set("band1Gain", p);
        }
    }
    // The shelves Premiere's equalizer has on by default stay flat: the one band is the whole filter.
    return m.take();
}

/// FCP7's Compressor/Limiter -> wabcut.audio.singleBandCompressor.
std::optional<MappedFilter> fcpCompressor(const FcpFilter& f) {
    Mapped m("wabcut.audio.singleBandCompressor", f.name.empty() ? std::string("Compressor/Limiter") : f.name);
    for (const FcpFilterParam& p : f.params) {
        const std::string k = paramKey(p);
        if (k == "threshold") {
            m.set("threshold", p);
        } else if (k == "ratio") {
            m.set("ratio", p);
        } else if (k == "attack" || k == "attacktime") {
            m.set("attack", p);
        } else if (k == "release" || k == "releasetime") {
            m.set("release", p);
        } else if (k == "gain" || k == "outputgain" || k == "makeup" || k == "makeupgain") {
            m.set("outputGain", p);
        } else if (k == "preservevolume" && p.value && p.value->constant() != 0.0) {
            m.approximated("Preserve Volume: Wabcut adds no make-up gain of its own (set Output Gain)");
        }
    }
    return m.take();
}

/// An effect Wabcut has under the filter's own name, its parameters by their names.
std::optional<MappedFilter> byName(const FcpFilter& f) {
    const std::string n = norm(f.name);
    const std::string id = effectKey(f);
    const EffectDescriptor* match = nullptr;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.intrinsic || (d.kind != EffectKind::Video && d.kind != EffectKind::Audio)) {
            continue;
        }
        if (!f.mediaType.empty() && (d.kind == EffectKind::Audio) != (toLowerAscii(f.mediaType) == "audio")) {
            continue;
        }
        const std::string dn = norm(d.displayName);
        if (!dn.empty() && (dn == n || dn == id)) {
            match = &d;
            break;
        }
    }
    if (!match) {
        return std::nullopt;
    }
    Mapped m(match->id, std::string(match->displayName));
    std::size_t carried = 0;
    std::vector<std::string> unknown;
    for (const FcpFilterParam& p : f.params) {
        const std::string k = paramKey(p);
        const EffectParamInfo* info = match->param(p.id);
        if (!info) {
            for (const EffectParamInfo& candidate : match->params) {
                if (norm(candidate.displayName) == k || norm(candidate.key) == k) {
                    info = &candidate;
                    break;
                }
            }
        }
        if (info && m.set(info->key, p)) {
            ++carried;
        } else if (!k.empty() && (p.value || !p.text.empty())) {
            unknown.push_back(p.name.empty() ? p.id : p.name);
        }
    }
    if (carried == 0 && !unknown.empty()) {
        return std::nullopt; // the same name, but not the same effect
    }
    for (const std::string& u : unknown) {
        m.dropped(u);
    }
    return m.take();
}

/// A filter Wabcut wrote beside another editor's: the Wabcut effect, exactly.
std::optional<MappedFilter> wabcutOwn(const FcpFilter& f) {
    const FcpFilterParam* id = nullptr;
    for (const FcpFilterParam& p : f.params) {
        if (p.id == kWabcutEffectParam) {
            id = &p;
        }
    }
    if (!id || id->text.empty() || !descriptorOf(id->text)) {
        return std::nullopt;
    }
    const EffectDescriptor* d = descriptorOf(id->text);
    Mapped m(d->id, std::string(d->displayName));
    for (const FcpFilterParam& p : f.params) {
        if (p.id.starts_with(kWabcutDataParam)) {
            const std::string key = p.id.substr(kWabcutDataParam.size());
            if (auto data = effectDataFromText(key, p.text)) {
                m.data(std::move(*data));
            }
        } else if (p.id != kWabcutEffectParam && p.id.starts_with(kWabcutParam)) {
            m.set(p.id.substr(kWabcutParam.size()), p);
        }
    }
    MappedFilter out = m.take();
    out.approximated.clear(); // exact by construction
    return out;
}

} // namespace

std::string effectDataToText(const EffectData& data) {
    if (const std::string* text = data.text()) {
        return "text:" + *text;
    }
    std::string out = "points:";
    if (const auto* points = data.points()) {
        for (std::size_t i = 0; i < points->size(); ++i) {
            char item[64];
            std::snprintf(item, sizeof item, "%s%.17g %.17g", i ? ";" : "", (*points)[i].x, (*points)[i].y);
            out += item;
        }
    }
    return out;
}

std::optional<EffectData> effectDataFromText(std::string key, std::string_view text) {
    if (key.empty() || key.size() > 128) {
        return std::nullopt;
    }
    if (text.starts_with("text:")) {
        text.remove_prefix(5);
        if (text.empty() || text.size() > kMaxEffectText || text.starts_with("asset:")) {
            return std::nullopt; // a project asset of another project
        }
        return EffectData{std::move(key), std::string(text)};
    }
    if (!text.starts_with("points:")) {
        return std::nullopt;
    }
    text.remove_prefix(7);
    std::vector<EffectPoint> points;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find(';'), text.size());
        const std::string_view item = text.substr(0, end);
        const std::size_t space = item.find(' ');
        double x = 0;
        double y = 0;
        if (space == std::string_view::npos ||
            std::from_chars(item.data(), item.data() + space, x).ec != std::errc{} ||
            std::from_chars(item.data() + space + 1, item.data() + item.size(), y).ec != std::errc{} || !std::isfinite(x) ||
            !std::isfinite(y) || points.size() >= kMaxEffectPoints) {
            return std::nullopt;
        }
        points.push_back({x, y});
        text.remove_prefix(std::min(end + 1, text.size()));
    }
    if (points.empty()) {
        return std::nullopt;
    }
    return EffectData{std::move(key), std::move(points)};
}

std::optional<ParamValue> premiereKeyList(std::string_view text) {
    text = trim(text);
    if (text.find(',') == std::string_view::npos) {
        return std::nullopt;
    }
    struct Key {
        std::int64_t time;
        double value;
    };
    std::vector<Key> keys;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find(';'), text.size());
        std::string_view item = trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (item.empty()) {
            continue; // a trailing ';'
        }
        const std::size_t c1 = item.find(',');
        if (c1 == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view timeText = trim(item.substr(0, c1));
        std::string_view valueText = item.substr(c1 + 1);
        valueText = trim(valueText.substr(0, valueText.find(',')));
        std::int64_t time = 0;
        if (const auto [p, ec] = std::from_chars(timeText.data(), timeText.data() + timeText.size(), time);
            ec != std::errc{} || p != timeText.data() + timeText.size()) {
            return std::nullopt;
        }
        double value = 0;
        if (iequals(valueText, "true") || iequals(valueText, "false")) {
            value = iequals(valueText, "true") ? 1.0 : 0.0;
        } else if (const auto [p, ec] = std::from_chars(valueText.data(), valueText.data() + valueText.size(), value);
                   ec != std::errc{} || p != valueText.data() + valueText.size() || !std::isfinite(value)) {
            return std::nullopt;
        }
        if (keys.size() >= 10'000) {
            break;
        }
        keys.push_back({time, value});
    }
    if (keys.empty()) {
        return std::nullopt;
    }
    // The static key (at -100 hours) stands for "no keyframes"; timed keys beside it are the animation.
    std::vector<Key> timed;
    std::copy_if(keys.begin(), keys.end(), std::back_inserter(timed), [](const Key& k) { return k.time > kPremiereStaticTime / 2; });
    if (timed.size() < 2) {
        return ParamValue(timed.empty() ? keys.front().value : timed.front().value);
    }
    Curve curve;
    for (const Key& k : timed) {
        const Keyframe key{Ticks{k.time}, k.value};
        if (const auto at = curve.keyAt(key.time)) {
            curve.keys[*at] = key;
        } else {
            curve.keys.insert(curve.keys.begin() + static_cast<std::ptrdiff_t>(curve.insertionIndex(key.time)), key);
        }
    }
    return ParamValue(std::move(curve));
}

std::string premiereKeyListText(const ParamValue& value, double scale, bool boolean) {
    const auto item = [&](std::int64_t time, double v) {
        std::string out = std::to_string(time) + ",";
        if (boolean) {
            out += v != 0.0 ? "true" : "false";
        } else {
            char text[64];
            const double x = v * scale;
            const auto [end, ec] = std::to_chars(text, text + sizeof text, std::isfinite(x) ? x : 0.0);
            out.append(text, ec == std::errc{} ? end : text);
        }
        return out + ",0,0,0,0,0,0";
    };
    if (const Curve* curve = value.curve(); curve && !curve->keys.empty()) {
        std::string out;
        for (const Keyframe& k : curve->keys) {
            out += (out.empty() ? "" : ";") + item(k.time.value, k.value);
        }
        return out;
    }
    return item(kPremiereStaticTime, value.constant());
}

std::optional<MappedFilter> mapFcpFilter(const FcpFilter& f) {
    if (auto own = wabcutOwn(f)) {
        return own;
    }
    if (named(f, {"lumetricolor", "lumetri"})) {
        return lumetri(f);
    }
    if (named(f, {"fastcolorcorrector", "colorcorrector"})) {
        return colorCorrector(f, false);
    }
    if (named(f, {"threewaycolorcorrector", "3waycolorcorrector", "colorcorrector3way", "3way"})) {
        return colorCorrector(f, true);
    }
    if (named(f, {"rgbcurves", "curves"})) {
        return rgbCurves(f);
    }
    if (named(f, {"3bandequalizer", "threebandequalizer", "3bandeq", "eq"})) {
        return threeBandEq(f);
    }
    if (named(f, {"parametricequalizer", "parametriceq"}) &&
        std::any_of(f.params.begin(), f.params.end(), [](const FcpFilterParam& p) { return paramKey(p) == "frequency"; })) {
        return singleBandEq(f);
    }
    if (named(f, {"compressorlimiter", "compressor"})) {
        return fcpCompressor(f);
    }
    return byName(f);
}

// --- Writing ---------------------------------------------------------------------------------------------------------

namespace {

const ParamValue* paramOf(const EffectInstance& e, std::string_view key) { return e.find(key); }

ParamValue valueOr(const EffectInstance& e, const EffectDescriptor* d, std::string_view key) {
    if (const ParamValue* v = paramOf(e, key)) {
        return *v;
    }
    const EffectParamInfo* info = d ? d->param(key) : nullptr;
    return ParamValue(info ? info->defaultValue : 0.0);
}

bool isDefault(const EffectInstance& e, const EffectDescriptor* d, std::string_view key) {
    const ParamValue* v = paramOf(e, key);
    const EffectParamInfo* info = d ? d->param(key) : nullptr;
    return !v || (v->isConstant() && info && v->constant() == info->defaultValue);
}

/// Premiere's effects Wabcut has under the same name (written with all their parameters).
constexpr std::string_view kClones[] = {
    "wabcut.audio.parametricEq", "wabcut.audio.dynamics", "wabcut.audio.singleBandCompressor",
    "wabcut.audio.multibandCompressor", "wabcut.audio.simpleParametricEq", "wabcut.audio.bass", "wabcut.audio.treble",
    "wabcut.audio.hardLimiter", "wabcut.adjust.levels", "wabcut.color.balance", "wabcut.adjust.procAmp",
};

} // namespace

std::optional<FcpWriteFilter> fcpFilterFor(const EffectInstance& e) {
    const EffectDescriptor* d = descriptorOf(e.effect);
    if (!d) {
        return std::nullopt;
    }
    FcpWriteFilter out;
    out.mediaType = d->kind == EffectKind::Audio ? "audio" : "video";
    auto native = [&](std::string id, std::string name, ParamValue v, double scale = 1.0) {
        FcpWriteParam p;
        p.id = std::move(id);
        p.name = std::move(name);
        p.value = std::move(v);
        p.scale = scale;
        out.params.push_back(std::move(p));
    };
    if (e.effect == "wabcut.color.grade" || e.effect == "wabcut.color.basic") {
        const bool grade = e.effect == "wabcut.color.grade";
        // PF: Lumetri Color the way Premiere writes it (checked against a Premiere export): effect id "Lumetri", an
        // empty name, no category, <pproBypass>, numeric parameter ids in Premiere's order, values as key lists.
        out.id = "Lumetri";
        out.premiereBypass = true;
        struct Row {
            const char* id;
            const char* key;
            const char* basicKey; ///< wabcut.color.basic's key, or null
            const char* name;
            bool toggle;          ///< a section switch (written true / false)
        };
        static constexpr Row kRows[] = {
            {"3", "basic.enabled", nullptr, " ", true},
            {"7", "basic.temperature", "temperature", "Temperature", false},
            {"8", "basic.tint", "tint", "Tint", false},
            {"20", "basic.saturation", "saturation", "Saturation", false},
            {"11", "basic.exposure", "exposure", "Exposure", false},
            {"12", "basic.contrast", "contrast", "Contrast", false},
            {"13", "basic.highlights", nullptr, "Highlights", false},
            {"14", "basic.shadows", nullptr, "Shadows", false},
            {"15", "basic.whites", nullptr, "Whites", false},
            {"16", "basic.blacks", nullptr, "Blacks", false},
            {"23", "creative.enabled", nullptr, " ", true},
            {"26", "creative.lookIntensity", nullptr, "Intensity", false},
            {"28", "creative.fadedFilm", nullptr, "Faded Film", false},
            {"30", "creative.vibrance", nullptr, "Vibrance", false},
            {"31", "creative.saturation", nullptr, "Saturation", false},
            {"33", "creative.tintBalance", nullptr, "Tint Balance", false},
            {"37", "curves.enabled", nullptr, " ", true},
            {"46", "wheels.enabled", nullptr, " ", true},
            {"50", "vignette.enabled", nullptr, " ", true},
            {"51", "vignette.amount", nullptr, "Amount", false},
            {"52", "vignette.midpoint", nullptr, "Midpoint", false},
            {"53", "vignette.roundness", nullptr, "Roundness", false},
            {"54", "vignette.feather", nullptr, "Feather", false},
        };
        for (const Row& r : kRows) {
            std::optional<ParamValue> v;
            if (grade) {
                v = valueOr(e, d, r.key);
            } else if (r.basicKey) {
                v = valueOr(e, d, r.basicKey);
            } else if (r.toggle) {
                v = ParamValue(1.0); // Basic Correction has no sections to switch off
            }
            if (v) {
                native(r.id, r.name, std::move(*v));
                out.params.back().premiereKeys = true;
                out.params.back().boolean = r.toggle;
            }
        }
        if (grade) {
            std::vector<std::string> wabcutOnly;
            for (const EffectParam& p : e.params) {
                const bool written = std::any_of(std::begin(kRows), std::end(kRows), [&](const Row& r) { return p.key == r.key; });
                if (!written && !isDefault(e, d, p.key) && !p.key.ends_with(".enabled")) {
                    // Premiere's Lumetri has Vibrance in Creative only: Wabcut's Basic vibrance has no slot.
                    const std::string stage = p.key == "basic.vibrance" ? std::string("basic vibrance") : p.key.substr(0, p.key.find('.'));
                    if (std::find(wabcutOnly.begin(), wabcutOnly.end(), stage) == wabcutOnly.end()) {
                        wabcutOnly.push_back(stage);
                    }
                }
            }
            for (const EffectData& data : e.data) {
                if (data.key == "basic.inputLut" || data.key == "creative.look") {
                    out.notes.push_back(std::string(data.key == "basic.inputLut" ? "the input LUT" : "the Look") +
                                        (data.text() ? " '" + *data.text() + "'" : std::string()) +
                                        " (FCP7 XML has no LUT path: only Wabcut reads it back)");
                } else if (std::find(wabcutOnly.begin(), wabcutOnly.end(), "curves") == wabcutOnly.end()) {
                    wabcutOnly.push_back("curves");
                }
            }
            if (!wabcutOnly.empty()) {
                std::string list;
                for (const std::string& s : wabcutOnly) {
                    list += (list.empty() ? "" : ", ") + s;
                }
                out.notes.push_back("Lumetri Color: its " + list + " settings are only read back by Wabcut");
            }
        }
    } else if (e.effect == "wabcut.audio.eq3") {
        // Premiere's Parametric Equalizer: the low and high shelves and band 1 are the three bands.
        const EffectDescriptor* peq = descriptorOf("wabcut.audio.parametricEq");
        out.id = "Parametric Equalizer";
        out.name = "Parametric Equalizer";
        out.category = "Filter and EQ";
        auto nameOf = [&](std::string_view key) { return std::string(peq && peq->param(key) ? peq->param(key)->displayName : key); };
        native("lowShelfEnabled", nameOf("lowShelfEnabled"), ParamValue(1.0));
        native("lowShelfFrequency", nameOf("lowShelfFrequency"), valueOr(e, d, "lowFrequency"));
        native("lowShelfGain", nameOf("lowShelfGain"), valueOr(e, d, "lowGain"));
        native("band1Frequency", nameOf("band1Frequency"), valueOr(e, d, "midFrequency"));
        native("band1Gain", nameOf("band1Gain"), valueOr(e, d, "midGain"));
        native("band1Q", nameOf("band1Q"), valueOr(e, d, "midQ"));
        native("highShelfEnabled", nameOf("highShelfEnabled"), ParamValue(1.0));
        native("highShelfFrequency", nameOf("highShelfFrequency"), valueOr(e, d, "highFrequency"));
        native("highShelfGain", nameOf("highShelfGain"), valueOr(e, d, "highGain"));
        out.notes.push_back("Parametric EQ (3-band) is written as Premiere's Parametric Equalizer (low shelf, band 1, "
                            "high shelf); other editors bring it back as that");
    } else if (std::find(std::begin(kClones), std::end(kClones), e.effect) != std::end(kClones)) {
        out.id = std::string(d->displayName);
        out.name = std::string(d->displayName);
        out.category = std::string(d->category);
        for (const EffectParamInfo& info : d->params) {
            native(std::string(info.key), std::string(info.displayName), valueOr(e, d, info.key));
        }
    } else {
        return std::nullopt;
    }
    // Every Wabcut parameter and curve, for Wabcut to read back exactly (other editors skip unknown parameters).
    FcpWriteParam effectId;
    effectId.id = std::string(kWabcutEffectParam);
    effectId.name = "Wabcut effect";
    effectId.text = e.effect;
    effectId.wabcutOnly = true;
    out.params.push_back(std::move(effectId));
    for (const EffectParam& p : e.params) {
        FcpWriteParam w;
        w.id = std::string(kWabcutParam) + p.key;
        w.name = "Wabcut " + p.key;
        w.value = p.value;
        w.wabcutOnly = true;
        out.params.push_back(std::move(w));
    }
    for (const EffectData& data : e.data) {
        FcpWriteParam w;
        w.id = std::string(kWabcutDataParam) + data.key;
        w.name = "Wabcut " + data.key;
        w.text = effectDataToText(data);
        w.wabcutOnly = true;
        out.params.push_back(std::move(w));
    }
    return out;
}

} // namespace wabcut::interchange::detail
