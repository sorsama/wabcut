#pragma once

// How an OpenFX plug-in's parameters become the effect parameters of its descriptor (OfxCatalog.h lists the mapping), and
// back: the values an instance sends the helper. Internal to wabcut_ofx.

#include <wabcut/ofxhost/OfxCatalog.h>
#include <wabcut/ofxhost/Protocol.h>

#include <wabcut/model/Effects.h>
#include <wabcut/model/Parameters.h>

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>
#include <vector>

namespace wabcut::ofxhost {

inline constexpr std::size_t kMaxOptionsKept = kMaxOptions;

/// The key suffixes of a kind's components (".r", ".g", ".b" for RGB; ".x", ".y" for 2D); empty for the kinds that hold one
/// value or text.
std::vector<const char*> componentSuffixes(OfxParamKind kind);

/// One effect parameter of a descriptor.
struct DescriptorParam {
    std::string key;
    std::string display;
    ParamUnit unit = ParamUnit::Amount;
    double defaultValue = 0.0;
    double minValue = 0.0;
    double maxValue = 1.0;
    bool animatable = true;
    std::string options; ///< a menu's labels, '|' separated
};

/// The effect parameters of one plug-in parameter (several for the multi-value kinds; none for a String, which is a text
/// setting).
std::vector<DescriptorParam> descriptorParams(const OfxParamInfo& param);

/// The parameter values the helper's render request carries, by the plug-in's parameter names: a number, an array of
/// numbers, or a string. `value(key)` gives the effect parameter's value at the frame's curve time, `text(key)` an effect
/// text setting.
nlohmann::json renderParams(const OfxPluginInfo& plugin, const std::function<double(const std::string&)>& value,
                            const std::function<std::string(const std::string&)>& text);

} // namespace wabcut::ofxhost
