#include "OfxParams.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace wabcut::ofxhost {

std::vector<const char*> componentSuffixes(OfxParamKind kind) {
    switch (kind) {
    case OfxParamKind::Rgb:
        return {"r", "g", "b"};
    case OfxParamKind::Rgba:
        return {"r", "g", "b", "a"};
    case OfxParamKind::Double2D:
    case OfxParamKind::Int2D:
        return {"x", "y"};
    case OfxParamKind::Double3D:
    case OfxParamKind::Int3D:
        return {"x", "y", "z"};
    default:
        return {};
    }
}

namespace {

double at(const std::vector<double>& v, std::size_t i) { return i < v.size() ? v[i] : 0.0; }

bool usable(double v) { return std::isfinite(v) && std::abs(v) < 1.0e8; }

/// The range the model keeps for component `i`: the plug-in's hard limits, else the range it shows, else a wide default;
/// a very wide hard range gives way to the display range (a slider's), and the default is always inside.
void rangeOf(const OfxParamInfo& p, std::size_t i, double& lo, double& hi, double& def) {
    const double mn = at(p.mins, i);
    const double mx = at(p.maxs, i);
    const double dmn = at(p.displayMins, i);
    const double dmx = at(p.displayMaxs, i);
    def = at(p.defaults, i);
    lo = usable(mn) ? mn : usable(dmn) ? dmn : -1000.0;
    hi = usable(mx) ? mx : usable(dmx) ? dmx : 1000.0;
    if (usable(dmn) && usable(dmx) && dmx > dmn && hi - lo > 100000.0) {
        lo = dmn;
        hi = dmx;
    }
    lo = std::min(lo, def);
    hi = std::max(hi, def);
    if (!(hi > lo)) {
        hi = lo + 1.0;
    }
}

} // namespace

std::vector<DescriptorParam> descriptorParams(const OfxParamInfo& p) {
    std::vector<DescriptorParam> out;
    const std::vector<std::string> keys = p.keys();
    const std::vector<const char*> suffixes = componentSuffixes(p.kind);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        DescriptorParam d;
        d.key = keys[i];
        d.display = p.label.empty() ? p.name : p.label;
        if (!suffixes.empty()) {
            std::string suffix = suffixes[i];
            std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            d.display += " " + suffix;
        }
        d.animatable = p.animates;
        switch (p.kind) {
        case OfxParamKind::Bool:
            d.unit = ParamUnit::Amount;
            d.defaultValue = at(p.defaults, 0) != 0.0 ? 1.0 : 0.0;
            d.minValue = 0.0;
            d.maxValue = 1.0;
            d.animatable = false;
            break;
        case OfxParamKind::Choice: {
            const double top = static_cast<double>(std::max<std::size_t>(p.options.size(), 1) - 1);
            d.unit = ParamUnit::Amount;
            d.minValue = 0.0;
            d.maxValue = top;
            d.defaultValue = std::clamp(std::round(at(p.defaults, 0)), 0.0, top);
            d.animatable = false;
            for (const std::string& o : p.options) {
                std::string label = o;
                std::replace(label.begin(), label.end(), '|', '/');
                d.options += (d.options.empty() ? "" : "|") + label;
            }
            break;
        }
        case OfxParamKind::Rgb:
        case OfxParamKind::Rgba:
            d.unit = ParamUnit::Fraction;
            d.minValue = 0.0;
            d.maxValue = 1.0;
            d.defaultValue = std::clamp(at(p.defaults, i), 0.0, 1.0);
            break;
        case OfxParamKind::Int:
        case OfxParamKind::Int2D:
        case OfxParamKind::Int3D: {
            d.unit = ParamUnit::Amount;
            rangeOf(p, i, d.minValue, d.maxValue, d.defaultValue);
            d.minValue = std::floor(d.minValue);
            d.maxValue = std::ceil(d.maxValue);
            d.defaultValue = std::round(d.defaultValue);
            break;
        }
        default: { // the Double kinds
            d.unit = p.angle ? ParamUnit::Degrees : p.scale ? ParamUnit::Factor : p.spatial ? ParamUnit::Fraction
                   : p.pixels ? ParamUnit::Pixels : ParamUnit::Amount;
            rangeOf(p, i, d.minValue, d.maxValue, d.defaultValue);
            if (p.spatial) { // a fraction of the picture: a little outside it is allowed
                d.minValue = std::min(d.minValue, -1.0);
                d.maxValue = std::max(d.maxValue, 2.0);
            } else if (p.pixels) {
                d.minValue = std::min(d.minValue, -8192.0);
                d.maxValue = std::max(d.maxValue, 8192.0);
            }
            break;
        }
        }
        out.push_back(std::move(d));
    }
    return out;
}

nlohmann::json renderParams(const OfxPluginInfo& plugin, const std::function<double(const std::string&)>& value,
                            const std::function<std::string(const std::string&)>& text) {
    nlohmann::json out = nlohmann::json::object();
    for (const OfxParamInfo& p : plugin.params) {
        if (p.kind == OfxParamKind::String) {
            out[p.name] = text(p.name);
            continue;
        }
        const std::vector<std::string> keys = p.keys();
        if (keys.size() == 1) {
            out[p.name] = value(keys[0]);
        } else {
            nlohmann::json a = nlohmann::json::array();
            for (const std::string& k : keys) {
                a.push_back(value(k));
            }
            out[p.name] = std::move(a);
        }
    }
    return out;
}

} // namespace wabcut::ofxhost
