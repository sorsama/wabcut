// wabcut-ofxhost: loading an OpenFX binary, describing its plug-ins, and rendering with them (0.7, docs/plugins.md).
//
// The action sequences follow the OpenFX Image Effect API: https://openfx.readthedocs.io/en/main/Reference/ofxImageEffectActions.html
//   scan:    load -> describe -> describeInContext (Filter, then General) -> unload
//   serve:   load -> describe -> describeInContext -> createInstance -> getClipPreferences, then per frame
//            beginInstanceChanged / instanceChanged / endInstanceChanged for the parameters that changed,
//            isIdentity, beginSequenceRender, render, endSequenceRender; destroyInstance and unload at the end.
// Pictures reach the plug-in as OpenFX images: the pixel depth and components it chose, rows from the bottom up.

#include "OfxHost.h"

#include <openfx/ofxMemory.h>

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using nlohmann::json;

namespace wabcut::ofxhost {

namespace {

using GetNumberFn = int (*)();
using GetPluginFn = OfxPlugin* (*)(int);
using SetHostFn = OfxStatus (*)(const OfxHost*);

constexpr int kMaxPlugins = 256;

std::string utf8Path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string win32Error(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

/// A plug-in's entry point, guarded against exceptions (a hard crash ends the process: the editor reports it).
OfxStatus callMain(const OfxPlugin* plugin, const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    try {
        return plugin->mainEntry(action, handle, in, out);
    } catch (...) {
        std::fprintf(stderr, "[ofxhost] %s threw an exception\n", action);
        return kOfxStatFailed;
    }
}

bool ok(OfxStatus s) { return s == kOfxStatOK; }
bool okOrDefault(OfxStatus s) { return s == kOfxStatOK || s == kOfxStatReplyDefault; }

std::string limited(std::string s, std::size_t max) {
    if (s.size() > max) {
        s.resize(max);
    }
    return s;
}

bool contains(const std::vector<std::string>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// Copies every property of `from` into `to` (a context descriptor starts with what describe set).
void copyBag(const PropertyBag& from, PropertyBag& to, const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        if (const auto* e = from.find(name)) {
            for (std::size_t i = 0; i < e->size(); ++i) {
                switch (e->kind) {
                case PropertyBag::Kind::Int:
                    to.setInt(name, e->ints[i], i);
                    break;
                case PropertyBag::Kind::Double:
                    to.setDouble(name, e->doubles[i], i);
                    break;
                case PropertyBag::Kind::String:
                    to.setString(name, e->strings[i], i);
                    break;
                case PropertyBag::Kind::Pointer:
                    to.setPointer(name, e->pointers[i], i);
                    break;
                }
            }
        }
    }
}

const std::vector<std::string>& descriptorPropertyNames() {
    static const std::vector<std::string> names = {kOfxPropLabel,
                                                   kOfxPropShortLabel,
                                                   kOfxPropLongLabel,
                                                   kOfxPropPluginDescription,
                                                   kOfxImageEffectPluginPropGrouping,
                                                   kOfxImageEffectPropSupportedContexts,
                                                   kOfxImageEffectPropSupportedPixelDepths,
                                                   kOfxImageEffectPluginPropSingleInstance,
                                                   kOfxImageEffectPluginRenderThreadSafety,
                                                   kOfxImageEffectPluginPropHostFrameThreading,
                                                   kOfxImageEffectPropSupportsMultiResolution,
                                                   kOfxImageEffectPropSupportsTiles,
                                                   kOfxImageEffectPropTemporalClipAccess,
                                                   kOfxImageEffectPropSupportsMultipleClipDepths,
                                                   kOfxImageEffectPropSupportsMultipleClipPARs,
                                                   kOfxImageEffectPluginPropFieldRenderTwiceAlways,
                                                   kOfxImageEffectPropClipPreferencesSlaveParam,
                                                   kOfxImageEffectPluginPropOverlayInteractV1,
                                                   kOfxPropVersion,
                                                   kOfxPropVersionLabel};
    return names;
}

std::string depthKey(std::string_view depth) {
    if (depth == kOfxBitDepthFloat) {
        return "float";
    }
    if (depth == kOfxBitDepthShort) {
        return "short";
    }
    if (depth == kOfxBitDepthByte) {
        return "byte";
    }
    return "none";
}

/// Why a described plug-in cannot be used here, or empty.
std::string unsupportedReason(PluginRuntime& rt) {
    const PropertyBag& d = rt.descriptor.props.bag;
    // GPU-only plug-ins: any "...RenderSupported" property the plug-in sets to "needed".
    for (const char* gpu : {"OfxImageEffectPropOpenGLRenderSupported", "OfxImageEffectPropCudaRenderSupported",
                            "OfxImageEffectPropCudaStreamSupported", "OfxImageEffectPropMetalRenderSupported",
                            "OfxImageEffectPropOpenCLRenderSupported"}) {
        if (d.getString(gpu) == "needed") {
            return std::string("needs GPU rendering (") + gpu + ")";
        }
    }
    if (rt.context.empty()) {
        return "supports no context this host runs (the Filter and General contexts are supported)";
    }
    return {};
}

/// Picks the context, pixel depth and components for a described plug-in and says whether the host can run it.
void choose(PluginRuntime& rt) {
    const PropertyBag& d = rt.descriptor.props.bag;
    const std::vector<std::string> contexts = d.strings(kOfxImageEffectPropSupportedContexts);
    rt.context.clear();
    for (const char* wanted : {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral}) {
        const auto it = rt.contexts.find(wanted);
        if (it == rt.contexts.end() || !contains(contexts, wanted)) {
            continue;
        }
        const Effect& c = *it->second;
        const Clip* source = c.find(kOfxImageEffectSimpleSourceClipName);
        const Clip* output = c.find(kOfxImageEffectOutputClipName);
        if (source == nullptr || output == nullptr) {
            continue;
        }
        bool others = false; // a clip this host has no picture for that the plug-in cannot do without
        for (const auto& clip : c.clips) {
            if (clip.get() != source && clip.get() != output && clip->props.bag.getInt(kOfxImageClipPropOptional, 0) == 0) {
                others = true;
            }
        }
        if (others) {
            continue;
        }
        rt.context = wanted;
        break;
    }
    // Depth: the best the plug-in and the host share. A plug-in that lists none gets Float.
    std::vector<std::string> depths = d.strings(kOfxImageEffectPropSupportedPixelDepths);
    rt.depth.clear();
    if (depths.empty()) {
        rt.depth = kOfxBitDepthFloat;
    } else {
        for (const char* wanted : {kOfxBitDepthFloat, kOfxBitDepthShort, kOfxBitDepthByte}) {
            if (contains(depths, wanted)) {
                rt.depth = wanted;
                break;
            }
        }
    }
    // Components: RGBA when the source takes it, else RGB.
    rt.components.clear();
    if (!rt.context.empty()) {
        const Effect& c = *rt.contexts.find(rt.context)->second;
        const std::vector<std::string> src = c.find(kOfxImageEffectSimpleSourceClipName)->props.bag.strings(kOfxImageEffectPropSupportedComponents);
        const std::vector<std::string> dst = c.find(kOfxImageEffectOutputClipName)->props.bag.strings(kOfxImageEffectPropSupportedComponents);
        for (const char* wanted : {kOfxImageComponentRGBA, kOfxImageComponentRGB}) {
            if (contains(src, wanted) && contains(dst, wanted)) {
                rt.components = wanted;
                break;
            }
        }
    }
    if (rt.problem.empty()) {
        rt.problem = unsupportedReason(rt);
    }
    if (rt.problem.empty() && rt.depth.empty()) {
        rt.problem = "needs a pixel depth this host does not provide (Float, Short and Byte are)";
    }
    if (rt.problem.empty() && rt.components.empty()) {
        rt.problem = "needs an image type this host does not provide (RGBA and RGB are)";
    }
}

void describePlugin(PluginRuntime& rt) {
    if (rt.described) {
        return;
    }
    rt.described = true;
    rt.descriptor.plugin = &rt;
    rt.descriptor.props.bag.setString(kOfxPropType, kOfxTypeImageEffect);
    rt.descriptor.props.bag.setPointer(kOfxImageEffectPropPluginHandle, rt.plugin);
    OfxStatus s = callMain(rt.plugin, kOfxActionLoad, nullptr, nullptr, nullptr);
    if (!okOrDefault(s)) {
        rt.problem = "the plug-in failed to load (status " + std::to_string(s) + ")";
        return;
    }
    rt.loaded = true;
    s = callMain(rt.plugin, kOfxActionDescribe, &rt.descriptor, nullptr, nullptr);
    if (!ok(s)) {
        rt.problem = "describe failed (status " + std::to_string(s) + ")";
        return;
    }
    const std::vector<std::string> contexts = rt.descriptor.props.bag.strings(kOfxImageEffectPropSupportedContexts);
    for (const char* wanted : {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral}) {
        if (!contains(contexts, wanted)) {
            continue;
        }
        auto effect = std::make_unique<Effect>();
        effect->plugin = &rt;
        effect->context = wanted;
        copyBag(rt.descriptor.props.bag, effect->props.bag, descriptorPropertyNames());
        effect->props.bag.setString(kOfxPropType, kOfxTypeImageEffect);
        effect->props.bag.setPointer(kOfxImageEffectPropPluginHandle, rt.plugin);
        OfxPropertySetStruct in;
        in.bag.setString(kOfxImageEffectPropContext, wanted);
        const OfxStatus c = callMain(rt.plugin, kOfxImageEffectActionDescribeInContext, effect.get(), &in, nullptr);
        if (ok(c)) {
            rt.contexts.emplace(wanted, std::move(effect));
        } else {
            std::fprintf(stderr, "[ofxhost] %s: describeInContext(%s) failed (status %d)\n", rt.id.c_str(), wanted, c);
        }
    }
    choose(rt);
}

json numbersJson(const std::vector<double>& v) {
    json a = json::array();
    for (const double x : v) {
        a.push_back(std::isfinite(x) ? x : (x < 0 ? -DBL_MAX : DBL_MAX));
    }
    return a;
}

json paramJson(const ParamDef& p) {
    const PropertyBag& b = p.props.bag;
    json j{{"name", limited(p.name, kMaxNameBytes)},
           {"kind", std::string(paramKindName(p.kind))},
           {"label", limited(b.getString(kOfxPropLabel, p.name), kMaxNameBytes)},
           {"hint", limited(b.getString(kOfxParamPropHint), kMaxTextBytes)},
           {"parent", limited(b.getString(kOfxParamPropParent), kMaxNameBytes)},
           {"animates", b.getInt(kOfxParamPropAnimates, 0) != 0},
           {"secret", b.getInt(kOfxParamPropSecret, 0) != 0}};
    if (paramIsNumeric(p.kind)) {
        j["default"] = numbersJson(b.numbers(kOfxParamPropDefault));
        j["min"] = numbersJson(b.numbers(kOfxParamPropMin));
        j["max"] = numbersJson(b.numbers(kOfxParamPropMax));
        j["dmin"] = numbersJson(b.numbers(kOfxParamPropDisplayMin));
        j["dmax"] = numbersJson(b.numbers(kOfxParamPropDisplayMax));
        j["increment"] = b.getDouble(kOfxParamPropIncrement, 1.0);
        j["digits"] = b.getInt(kOfxParamPropDigits, 2);
        if (b.has(kOfxParamPropDoubleType)) {
            j["doubleType"] = b.getString(kOfxParamPropDoubleType);
            j["coords"] = b.getString(kOfxParamPropDefaultCoordinateSystem, kOfxParamCoordinatesCanonical);
        }
    }
    if (p.kind == ParamKind::Choice || p.kind == ParamKind::StrChoice) {
        json options = json::array();
        for (const std::string& o : b.strings(kOfxParamPropChoiceOption)) {
            if (options.size() < kMaxOptions) {
                options.push_back(limited(o, kMaxNameBytes));
            }
        }
        j["options"] = std::move(options);
        if (p.kind == ParamKind::StrChoice) {
            json enums = json::array();
            for (const std::string& e : b.strings(kOfxParamPropChoiceEnum)) {
                if (enums.size() < kMaxOptions) {
                    enums.push_back(limited(e, kMaxNameBytes));
                }
            }
            j["enums"] = std::move(enums);
            j["default"] = limited(b.getString(kOfxParamPropDefault), kMaxNameBytes);
        } else {
            j["default"] = numbersJson(b.numbers(kOfxParamPropDefault));
        }
    }
    if (p.kind == ParamKind::Bool) {
        j["default"] = numbersJson(b.numbers(kOfxParamPropDefault));
    }
    if (p.kind == ParamKind::String) {
        j["default"] = limited(b.getString(kOfxParamPropDefault), kMaxTextBytes);
        j["stringMode"] = b.getString(kOfxParamPropStringMode, kOfxParamStringIsSingleLine);
    }
    return j;
}

/// Parameter names in the order the plug-in's pages give them, then the rest in declaration order.
std::vector<std::string> paramOrder(const Effect& context) {
    std::vector<std::string> order;
    const auto add = [&](const std::string& name) {
        if (name == kOfxParamPageSkipRow || name == kOfxParamPageSkipColumn || contains(order, name)) {
            return;
        }
        const ParamDef* p = context.params.find(name);
        if (p != nullptr && p->kind != ParamKind::Page && p->kind != ParamKind::Group) {
            order.push_back(name);
        }
    };
    for (const std::string& pageName : context.props.bag.strings(kOfxPluginPropParamPageOrder)) {
        if (const ParamDef* page = context.params.find(pageName); page != nullptr && page->kind == ParamKind::Page) {
            for (const std::string& child : page->props.bag.strings(kOfxParamPropPageChild)) {
                add(child);
            }
        }
    }
    for (const auto& p : context.params.params) {
        add(p->name);
    }
    return order;
}

std::vector<double> numbersFromJson(const json& v) {
    std::vector<double> out;
    if (v.is_number()) {
        out.push_back(v.get<double>());
    } else if (v.is_boolean()) {
        out.push_back(v.get<bool>() ? 1.0 : 0.0);
    } else if (v.is_array()) {
        for (const json& e : v) {
            if (e.is_number()) {
                out.push_back(e.get<double>());
            } else if (e.is_boolean()) {
                out.push_back(e.get<bool>() ? 1.0 : 0.0);
            }
        }
    }
    return out;
}

} // namespace

// ---- OfxModule ----------------------------------------------------------------------------------------------------

std::unique_ptr<OfxModule> OfxModule::load(const std::filesystem::path& file, std::string& error) {
    HMODULE h = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h == nullptr) {
        error = utf8Path(file) + ": " + win32Error("LoadLibrary");
        return nullptr;
    }
    const auto count = reinterpret_cast<GetNumberFn>(GetProcAddress(h, "OfxGetNumberOfPlugins"));
    const auto get = reinterpret_cast<GetPluginFn>(GetProcAddress(h, "OfxGetPlugin"));
    const auto setHost = reinterpret_cast<SetHostFn>(GetProcAddress(h, "OfxSetHost"));
    if (count == nullptr || get == nullptr) {
        error = utf8Path(file) + ": not an OpenFX plug-in (OfxGetPlugin is not exported)";
        FreeLibrary(h);
        return nullptr;
    }
    if (setHost != nullptr) {
        const OfxStatus s = setHost(hostStruct());
        if (s != kOfxStatOK) {
            error = utf8Path(file) + ": the plug-in declined this host";
            FreeLibrary(h);
            return nullptr;
        }
    }
    std::unique_ptr<OfxModule> module(new OfxModule());
    module->m_library = h;
    const int n = std::clamp(count(), 0, kMaxPlugins);
    for (int i = 0; i < n; ++i) {
        OfxPlugin* plugin = get(i);
        if (plugin == nullptr || plugin->pluginIdentifier == nullptr || plugin->mainEntry == nullptr || plugin->setHost == nullptr) {
            continue;
        }
        if (plugin->pluginApi == nullptr || std::string_view(plugin->pluginApi) != kOfxImageEffectPluginApi || plugin->apiVersion != 1) {
            continue; // not an image effect (OpenFX has no other API in use)
        }
        auto rt = std::make_unique<PluginRuntime>();
        rt->plugin = plugin;
        rt->id = limited(plugin->pluginIdentifier, kMaxNameBytes);
        plugin->setHost(const_cast<OfxHost*>(hostStruct()));
        module->m_plugins.push_back(std::move(rt));
    }
    return module;
}

OfxModule::~OfxModule() {
    for (auto& rt : m_plugins) {
        if (rt->loaded) {
            callMain(rt->plugin, kOfxActionUnload, nullptr, nullptr, nullptr);
        }
    }
    m_plugins.clear();
    if (m_library != nullptr) {
        FreeLibrary(static_cast<HMODULE>(m_library));
    }
}

PluginRuntime* OfxModule::find(std::string_view id) {
    for (auto& rt : m_plugins) {
        if (rt->id == id) {
            return rt.get();
        }
    }
    return nullptr;
}

void OfxModule::describeAll() {
    for (auto& rt : m_plugins) {
        describePlugin(*rt);
    }
}

json OfxModule::describeJson() {
    describeAll();
    json plugins = json::array();
    for (auto& rt : m_plugins) {
        const PropertyBag& d = rt->descriptor.props.bag;
        json p{{"id", rt->id},
               {"name", limited(d.getString(kOfxPropLabel, d.getString(kOfxPropShortLabel, rt->id)), kMaxNameBytes)},
               {"group", limited(d.getString(kOfxImageEffectPluginPropGrouping), kMaxNameBytes)},
               {"description", limited(d.getString(kOfxPropPluginDescription), kMaxTextBytes)},
               {"version", std::to_string(rt->plugin->pluginVersionMajor) + "." + std::to_string(rt->plugin->pluginVersionMinor)},
               {"versionLabel", limited(d.getString(kOfxPropVersionLabel), kMaxNameBytes)},
               {"context", rt->context == kOfxImageEffectContextFilter ? "filter" : rt->context == kOfxImageEffectContextGeneral ? "general" : ""},
               {"depth", rt->depth.empty() ? "" : depthKey(rt->depth)},
               {"components", rt->components == kOfxImageComponentRGBA ? "rgba" : rt->components == kOfxImageComponentRGB ? "rgb" : ""},
               {"singleInstance", d.getInt(kOfxImageEffectPluginPropSingleInstance, 0) != 0},
               {"usable", rt->problem.empty()}};
        if (!rt->problem.empty()) {
            p["problem"] = limited(rt->problem, kMaxTextBytes);
        }
        json params = json::array();
        if (const auto it = rt->contexts.find(rt->context); it != rt->contexts.end()) {
            const Effect& context = *it->second;
            for (const std::string& name : paramOrder(context)) {
                const ParamDef* def = context.params.find(name);
                if (def != nullptr && params.size() < kMaxParams) {
                    params.push_back(paramJson(*def));
                }
            }
        }
        p["params"] = std::move(params);
        plugins.push_back(std::move(p));
    }
    return plugins;
}

// ---- OfxInstance --------------------------------------------------------------------------------------------------

namespace {

int bytesPerSample(std::string_view depth) { return depth == kOfxBitDepthFloat ? 4 : depth == kOfxBitDepthShort ? 2 : 1; }

/// The editor's picture (float RGBA premultiplied, top-down) as the plug-in's image: its depth and components, rows from
/// the bottom up.
void toPluginImage(const float* src, int w, int h, std::string_view depth, bool rgba, bool hasAlpha, std::vector<std::uint8_t>& out) {
    const int channels = rgba ? 4 : 3;
    const std::size_t rowBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(channels) * static_cast<std::size_t>(bytesPerSample(depth));
    out.assign(rowBytes * static_cast<std::size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        const float* row = src + static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) * 4;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * rowBytes;
        for (int x = 0; x < w; ++x) {
            const float* px = row + static_cast<std::size_t>(x) * 4;
            for (int c = 0; c < channels; ++c) {
                float v = px[c];
                if (c == 3 && !hasAlpha) {
                    v = 1.0f;
                }
                const std::size_t i = static_cast<std::size_t>(x) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c);
                if (depth == kOfxBitDepthFloat) {
                    std::memcpy(dst + i * 4, &v, 4);
                } else if (depth == kOfxBitDepthShort) {
                    const auto s = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
                    std::memcpy(dst + i * 2, &s, 2);
                } else {
                    dst[i] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                }
            }
        }
    }
}

/// The plug-in's output image back to the editor's picture (float RGBA premultiplied, top-down).
void fromPluginImage(const std::vector<std::uint8_t>& in, int w, int h, std::string_view depth, bool rgba, OutputAlpha alpha, float* dst) {
    const int channels = rgba ? 4 : 3;
    const std::size_t rowBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(channels) * static_cast<std::size_t>(bytesPerSample(depth));
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* row = in.data() + static_cast<std::size_t>(y) * rowBytes;
        float* out = dst + static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) * 4;
        for (int x = 0; x < w; ++x) {
            float px[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (int c = 0; c < channels; ++c) {
                const std::size_t i = static_cast<std::size_t>(x) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c);
                if (depth == kOfxBitDepthFloat) {
                    std::memcpy(&px[c], row + i * 4, 4);
                } else if (depth == kOfxBitDepthShort) {
                    std::uint16_t s = 0;
                    std::memcpy(&s, row + i * 2, 2);
                    px[c] = static_cast<float>(s) * (1.0f / 65535.0f);
                } else {
                    px[c] = static_cast<float>(row[i]) * (1.0f / 255.0f);
                }
            }
            // A plug-in's NaN or infinity becomes black; the picture is bounded to what the editor can show.
            for (float& v : px) {
                v = std::isfinite(v) ? std::clamp(v, 0.0f, 16.0f) : 0.0f;
            }
            if (alpha == OutputAlpha::Opaque || !rgba) {
                px[3] = 1.0f;
            } else if (alpha == OutputAlpha::Unpremultiplied) {
                px[0] *= px[3];
                px[1] *= px[3];
                px[2] *= px[3];
            }
            std::memcpy(out + static_cast<std::size_t>(x) * 4, px, sizeof px);
        }
    }
}

void setImageProps(Image& img, int w, int h, std::string_view depth, bool rgba, std::string_view premult, double par) {
    PropertyBag& b = img.props.bag;
    const int channels = rgba ? 4 : 3;
    b.setString(kOfxPropType, kOfxTypeImage);
    b.setString(kOfxImageEffectPropPixelDepth, depth);
    b.setString(kOfxImageEffectPropComponents, rgba ? kOfxImageComponentRGBA : kOfxImageComponentRGB);
    b.setString(kOfxImageEffectPropPreMultiplication, premult);
    b.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    b.setDouble(kOfxImagePropPixelAspectRatio, par);
    b.setPointer(kOfxImagePropData, img.pixels.data());
    b.setInts(kOfxImagePropBounds, {0, 0, w, h});
    b.setInts(kOfxImagePropRegionOfDefinition, {0, 0, w, h});
    b.setInt(kOfxImagePropRowBytes, w * channels * bytesPerSample(depth));
    b.setString(kOfxImagePropField, kOfxImageFieldNone);
    b.setString(kOfxImagePropUniqueIdentifier, "wabcut-" + std::to_string(reinterpret_cast<std::uintptr_t>(&img)));
}

bool spatialType(const std::string& t) {
    return t == kOfxParamDoubleTypeX || t == kOfxParamDoubleTypeY || t == kOfxParamDoubleTypeXY || t == kOfxParamDoubleTypeXAbsolute ||
           t == kOfxParamDoubleTypeYAbsolute || t == kOfxParamDoubleTypeXYAbsolute;
}

} // namespace

std::unique_ptr<OfxInstance> OfxInstance::create(PluginRuntime& plugin, int width, int height, double fps, std::string& error) {
    if (!plugin.loaded || plugin.context.empty() || !plugin.problem.empty()) {
        error = plugin.problem.empty() ? "the plug-in is not usable" : plugin.problem;
        return nullptr;
    }
    const auto ctx = plugin.contexts.find(plugin.context);
    if (ctx == plugin.contexts.end()) {
        error = "the plug-in has no usable context";
        return nullptr;
    }
    if (plugin.descriptor.props.bag.getInt(kOfxImageEffectPluginPropSingleInstance, 0) != 0 && plugin.instances > 0) {
        error = "the plug-in allows one instance at a time";
        return nullptr;
    }
    const Effect& desc = *ctx->second;
    std::unique_ptr<OfxInstance> inst(new OfxInstance());
    inst->m_plugin = &plugin;
    Effect& e = inst->m_effect;
    e.instance = true;
    e.plugin = &plugin;
    e.context = plugin.context;
    e.width = width;
    e.height = height;
    PropertyBag& b = e.props.bag;
    b.setString(kOfxPropType, kOfxTypeImageEffectInstance);
    b.setString(kOfxImageEffectPropContext, plugin.context);
    b.setPointer(kOfxImageEffectPropPluginHandle, plugin.plugin);
    b.setDoubles(kOfxImageEffectPropProjectSize, {static_cast<double>(width), static_cast<double>(height)});
    b.setDoubles(kOfxImageEffectPropProjectOffset, {0.0, 0.0});
    b.setDoubles(kOfxImageEffectPropProjectExtent, {static_cast<double>(width), static_cast<double>(height)});
    b.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, 1.0);
    b.setDouble(kOfxImageEffectInstancePropEffectDuration, 1.0e6);
    b.setDouble(kOfxImageEffectPropFrameRate, fps);
    b.setInt(kOfxImageEffectInstancePropSequentialRender, 0);
    b.setInt(kOfxPropIsInteractive, 0);
    b.setInt(kOfxImageEffectPropSupportsTiles, 0);
    b.setString("OfxImageEffectPropOpenGLRenderSupported", "false");
    copyBag(desc.props.bag, b, descriptorPropertyNames());
    b.setString(kOfxPropType, kOfxTypeImageEffectInstance);
    // The clips: the descriptor's, with the picture format this host gives them.
    for (const auto& dc : desc.clips) {
        auto clip = std::make_unique<Clip>();
        clip->name = dc->name;
        clip->owner = &e;
        clip->props.bag.setString(kOfxPropType, kOfxTypeClip);
        clip->props.bag.setString(kOfxPropName, dc->name);
        clip->props.bag.setString(kOfxPropLabel, dc->props.bag.getString(kOfxPropLabel, dc->name));
        clip->props.bag.setInt(kOfxImageClipPropOptional, dc->props.bag.getInt(kOfxImageClipPropOptional, 0));
        clip->props.bag.setInt(kOfxImageClipPropIsMask, dc->props.bag.getInt(kOfxImageClipPropIsMask, 0));
        const bool connected = dc->name == kOfxImageEffectSimpleSourceClipName || dc->name == kOfxImageEffectOutputClipName;
        clip->props.bag.setInt(kOfxImageClipPropConnected, connected ? 1 : 0);
        clip->props.bag.setString(kOfxImageEffectPropPixelDepth, plugin.depth);
        clip->props.bag.setString(kOfxImageEffectPropComponents, plugin.components);
        clip->props.bag.setString(kOfxImageClipPropUnmappedPixelDepth, plugin.depth);
        clip->props.bag.setString(kOfxImageClipPropUnmappedComponents, plugin.components);
        clip->props.bag.setDouble(kOfxImagePropPixelAspectRatio, 1.0);
        clip->props.bag.setDouble(kOfxImageEffectPropFrameRate, fps);
        clip->props.bag.setDouble(kOfxImageEffectPropUnmappedFrameRate, fps);
        clip->props.bag.setDoubles(kOfxImageEffectPropFrameRange, {0.0, 1.0e6});
        clip->props.bag.setDoubles(kOfxImageEffectPropUnmappedFrameRange, {0.0, 1.0e6});
        clip->props.bag.setString(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
        clip->props.bag.setInt(kOfxImageClipPropContinuousSamples, 0);
        clip->props.bag.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
        e.clips.push_back(std::move(clip));
    }
    // The parameters: the descriptor's definitions, at their defaults.
    for (const auto& dp : desc.params.params) {
        auto p = std::make_unique<ParamDef>();
        p->name = dp->name;
        p->kind = dp->kind;
        copyBag(dp->props.bag, p->props.bag,
                {kOfxPropType, kOfxParamPropType, kOfxPropName, kOfxPropLabel, kOfxParamPropSecret, kOfxParamPropEnabled,
                 kOfxParamPropEvaluateOnChange, kOfxParamPropCanUndo, kOfxParamPropPersistant, kOfxParamPropPluginMayWrite,
                 kOfxParamPropAnimates, kOfxParamPropHint, kOfxParamPropParent, kOfxParamPropScriptName, kOfxParamPropDefault,
                 kOfxParamPropMin, kOfxParamPropMax, kOfxParamPropDisplayMin, kOfxParamPropDisplayMax, kOfxParamPropIncrement,
                 kOfxParamPropDigits, kOfxParamPropDoubleType, kOfxParamPropDefaultCoordinateSystem, kOfxParamPropStringMode,
                 kOfxParamPropChoiceOption, kOfxParamPropChoiceEnum, kOfxParamPropPageChild, kOfxParamPropGroupOpen,
                 kOfxParamPropDimensionLabel, kOfxParamPropDataPtr});
        if (paramIsNumeric(p->kind)) {
            p->numbers = dp->props.bag.numbers(kOfxParamPropDefault);
            p->numbers.resize(static_cast<std::size_t>(paramDimension(p->kind)), 0.0);
            // A default in normalised coordinates is a fraction of the project's size in canonical pixels.
            if (p->props.bag.getString(kOfxParamPropDefaultCoordinateSystem) == kOfxParamCoordinatesNormalised &&
                spatialType(p->props.bag.getString(kOfxParamPropDoubleType))) {
                for (std::size_t i = 0; i < p->numbers.size(); ++i) {
                    p->numbers[i] *= (i % 2 == 0) ? width : height;
                }
            }
        } else if (p->kind == ParamKind::String || p->kind == ParamKind::StrChoice) {
            p->text = dp->props.bag.getString(kOfxParamPropDefault);
        }
        e.params.params.push_back(std::move(p));
    }
    const OfxStatus created = callMain(plugin.plugin, kOfxActionCreateInstance, &e, nullptr, nullptr);
    if (!ok(created) && created != kOfxStatReplyDefault) {
        error = e.lastMessage.empty() ? "createInstance failed (status " + std::to_string(created) + ")" : e.lastMessage;
        return nullptr;
    }
    inst->m_created = true;
    ++plugin.instances;
    // Clip preferences: what the plug-in wants the clips to be.
    OfxPropertySetStruct prefs;
    for (const char* clip : {kOfxImageEffectSimpleSourceClipName, kOfxImageEffectOutputClipName}) {
        prefs.bag.setString(std::string("OfxImageClipPropComponents_") + clip, plugin.components);
        prefs.bag.setString(std::string("OfxImageClipPropDepth_") + clip, plugin.depth);
        prefs.bag.setDouble(std::string("OfxImageClipPropPAR_") + clip, 1.0);
    }
    prefs.bag.setDouble(kOfxImageEffectPropFrameRate, fps);
    prefs.bag.setString(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
    prefs.bag.setString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
    prefs.bag.setInt(kOfxImageClipPropContinuousSamples, 0);
    prefs.bag.setInt(kOfxImageEffectFrameVarying, 0);
    const OfxStatus pref = callMain(plugin.plugin, kOfxImageEffectActionGetClipPreferences, &e, nullptr, &prefs);
    if (ok(pref)) {
        const std::string comps = prefs.bag.getString(std::string("OfxImageClipPropComponents_") + kOfxImageEffectOutputClipName);
        const std::string depth = prefs.bag.getString(std::string("OfxImageClipPropDepth_") + kOfxImageEffectOutputClipName);
        const std::string srcDepth = prefs.bag.getString(std::string("OfxImageClipPropDepth_") + kOfxImageEffectSimpleSourceClipName);
        if (comps != plugin.components || depth != plugin.depth || srcDepth != plugin.depth) {
            error = "the plug-in asked for a picture format this host does not provide in its clip preferences";
            inst.reset();
            return nullptr;
        }
        inst->m_frameVarying = prefs.bag.getInt(kOfxImageEffectFrameVarying, 0) != 0 || prefs.bag.getInt(kOfxImageClipPropContinuousSamples, 0) != 0;
        const std::string premult = prefs.bag.getString(kOfxImageEffectPropPreMultiplication, kOfxImagePreMultiplied);
        inst->m_outputAlpha = premult == kOfxImageOpaque ? OutputAlpha::Opaque
                              : premult == kOfxImageUnPreMultiplied ? OutputAlpha::Unpremultiplied
                                                                    : OutputAlpha::Premultiplied;
    }
    return inst;
}

OfxInstance::~OfxInstance() {
    if (m_created && m_plugin != nullptr) {
        callMain(m_plugin->plugin, kOfxActionDestroyInstance, &m_effect, nullptr, nullptr);
        --m_plugin->instances;
    }
}

bool OfxInstance::applyParams(const json& params, double time, std::string& error) {
    if (!params.is_object()) {
        return true;
    }
    std::vector<std::string> changed;
    for (const auto& [name, value] : params.items()) {
        ParamDef* p = m_effect.params.find(name);
        if (p == nullptr) {
            continue; // a parameter this plug-in does not have (an old project): ignored
        }
        std::vector<double> nums;
        std::string text;
        if (paramIsNumeric(p->kind)) {
            nums = numbersFromJson(value);
            nums.resize(static_cast<std::size_t>(paramDimension(p->kind)), 0.0);
            for (double& v : nums) {
                if (!std::isfinite(v)) {
                    v = 0.0;
                }
            }
            if (p->kind == ParamKind::Int || p->kind == ParamKind::Int2D || p->kind == ParamKind::Int3D || p->kind == ParamKind::Choice) {
                for (double& v : nums) {
                    v = std::round(v);
                }
            }
            if (p->kind == ParamKind::Bool) {
                nums[0] = nums[0] != 0.0 ? 1.0 : 0.0;
            }
            // A normalised spatial parameter (a position as a fraction of the picture) reaches the plug-in in canvas pixels.
            const bool spatial = (p->kind == ParamKind::Double2D || p->kind == ParamKind::Double) &&
                                 spatialType(p->props.bag.getString(kOfxParamPropDoubleType)) &&
                                 p->props.bag.getString(kOfxParamPropDefaultCoordinateSystem) == kOfxParamCoordinatesNormalised;
            if (spatial) {
                const std::string t = p->props.bag.getString(kOfxParamPropDoubleType);
                for (std::size_t i = 0; i < nums.size(); ++i) {
                    const bool isY = t == kOfxParamDoubleTypeY || t == kOfxParamDoubleTypeYAbsolute || (nums.size() == 2 && i == 1);
                    nums[i] *= isY ? m_effect.height : m_effect.width;
                }
            }
        } else if (p->kind == ParamKind::String || p->kind == ParamKind::StrChoice) {
            if (value.is_string()) {
                text = limited(value.get<std::string>(), kMaxTextBytes);
            } else if (p->kind == ParamKind::StrChoice) {
                // A choice by index: the enum text of that option.
                const std::vector<double> idx = numbersFromJson(value);
                const std::vector<std::string> enums = p->props.bag.strings(kOfxParamPropChoiceEnum);
                const std::vector<std::string> options = p->props.bag.strings(kOfxParamPropChoiceOption);
                const std::vector<std::string>& names = enums.empty() ? options : enums;
                if (!idx.empty() && idx[0] >= 0 && static_cast<std::size_t>(idx[0]) < names.size()) {
                    text = names[static_cast<std::size_t>(idx[0])];
                }
            }
        } else {
            continue; // groups, pages, buttons and the kinds this host does not edit
        }
        const auto last = m_lastApplied.find(name);
        const bool same = last != m_lastApplied.end() && last->second.first == nums && last->second.second == text;
        if (same) {
            continue;
        }
        const bool first = last == m_lastApplied.end();
        m_lastApplied[name] = {nums, text};
        // The first time a value equals the plug-in's own default there is nothing to tell it.
        if (first && p->numbers == nums && p->text == text) {
            continue;
        }
        p->numbers = std::move(nums);
        p->text = std::move(text);
        changed.push_back(name);
    }
    if (changed.empty()) {
        return true;
    }
    OfxPropertySetStruct begin;
    begin.bag.setString(kOfxPropChangeReason, kOfxChangeUserEdited);
    callMain(m_plugin->plugin, kOfxActionBeginInstanceChanged, &m_effect, &begin, nullptr);
    for (const std::string& name : changed) {
        OfxPropertySetStruct in;
        in.bag.setString(kOfxPropType, kOfxTypeParameter);
        in.bag.setString(kOfxPropName, name);
        in.bag.setString(kOfxPropChangeReason, kOfxChangeUserEdited);
        in.bag.setDouble(kOfxPropTime, time);
        in.bag.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        const OfxStatus s = callMain(m_plugin->plugin, kOfxActionInstanceChanged, &m_effect, &in, nullptr);
        if (!okOrDefault(s) && s != kOfxStatReplyNo) {
            std::fprintf(stderr, "[ofxhost] instanceChanged(%s) returned %d\n", name.c_str(), s);
        }
    }
    OfxPropertySetStruct end;
    end.bag.setString(kOfxPropChangeReason, kOfxChangeUserEdited);
    callMain(m_plugin->plugin, kOfxActionEndInstanceChanged, &m_effect, &end, nullptr);
    (void)error;
    return true;
}

bool OfxInstance::render(const RenderArgs& a, const float* input, float* output, std::string& error, OutputAlpha& alpha) {
    Effect& e = m_effect;
    if (a.width <= 0 || a.height <= 0 || a.width > kMaxFrameSide || a.height > kMaxFrameSide) {
        error = "the picture has an unsupported size";
        return false;
    }
    e.width = a.width;
    e.height = a.height;
    e.time = a.time;
    e.lastMessage.clear();
    e.props.bag.setDoubles(kOfxImageEffectPropProjectSize, {static_cast<double>(a.width), static_cast<double>(a.height)});
    e.props.bag.setDoubles(kOfxImageEffectPropProjectExtent, {static_cast<double>(a.width), static_cast<double>(a.height)});
    e.props.bag.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, a.pixelAspect);
    e.props.bag.setDouble(kOfxImageEffectPropFrameRate, a.fps);
    if (a.params != nullptr && !applyParams(*a.params, a.time, error)) {
        return false;
    }
    const bool rgba = m_plugin->components == kOfxImageComponentRGBA;
    const std::string& depth = m_plugin->depth;
    const std::string_view premult = a.hasAlpha ? kOfxImagePreMultiplied : kOfxImageOpaque;
    Image source;
    Image result;
    source.owner = &e;
    result.owner = &e;
    toPluginImage(input, a.width, a.height, depth, rgba, a.hasAlpha, source.pixels);
    result.pixels.assign(source.pixels.size(), 0);
    setImageProps(source, a.width, a.height, depth, rgba, premult, a.pixelAspect);
    setImageProps(result, a.width, a.height, depth, rgba,
                  m_outputAlpha == OutputAlpha::Opaque ? kOfxImageOpaque : m_outputAlpha == OutputAlpha::Unpremultiplied ? kOfxImageUnPreMultiplied : kOfxImagePreMultiplied,
                  a.pixelAspect);
    for (const auto& clip : e.clips) {
        clip->props.bag.setPointer("wabcut.image", nullptr);
        clip->props.bag.setDoubles(kOfxImageEffectPropFrameRange, {0.0, 1.0e6});
        clip->props.bag.setDouble(kOfxImageEffectPropFrameRate, a.fps);
        clip->props.bag.setString(kOfxImageEffectPropPreMultiplication, clip->name == kOfxImageEffectOutputClipName ? (m_outputAlpha == OutputAlpha::Opaque ? kOfxImageOpaque : m_outputAlpha == OutputAlpha::Unpremultiplied ? kOfxImageUnPreMultiplied : kOfxImagePreMultiplied) : std::string(premult));
        clip->props.bag.setDouble(kOfxImagePropPixelAspectRatio, a.pixelAspect);
    }
    if (Clip* c = e.find(kOfxImageEffectSimpleSourceClipName)) {
        c->props.bag.setPointer("wabcut.image", &source);
    }
    if (Clip* c = e.find(kOfxImageEffectOutputClipName)) {
        c->props.bag.setPointer("wabcut.image", &result);
    }
    alpha = m_outputAlpha;

    const auto renderArgs = [&](OfxPropertySetStruct& in) {
        in.bag.setDouble(kOfxPropTime, a.time);
        in.bag.setString(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
        in.bag.setInts(kOfxImageEffectPropRenderWindow, {0, 0, a.width, a.height});
        in.bag.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        in.bag.setInt(kOfxImageEffectPropSequentialRenderStatus, 0);
        in.bag.setInt(kOfxImageEffectPropInteractiveRenderStatus, 0);
        in.bag.setInt(kOfxImageEffectPropRenderQualityDraft, 0);
    };

    // isIdentity: the plug-in may say the picture passes through unchanged.
    {
        OfxPropertySetStruct in;
        OfxPropertySetStruct out;
        renderArgs(in);
        out.bag.setString(kOfxPropName, "");
        out.bag.setDouble(kOfxPropTime, a.time);
        const OfxStatus s = callMain(m_plugin->plugin, kOfxImageEffectActionIsIdentity, &e, &in, &out);
        if (ok(s) && out.bag.getString(kOfxPropName) == kOfxImageEffectSimpleSourceClipName) {
            std::memcpy(output, input, static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height) * 4 * sizeof(float));
            if (!a.hasAlpha) {
                const std::size_t n = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
                for (std::size_t i = 0; i < n; ++i) {
                    output[i * 4 + 3] = 1.0f;
                }
            }
            alpha = a.hasAlpha ? OutputAlpha::Premultiplied : OutputAlpha::Opaque;
            return true;
        }
    }
    {
        OfxPropertySetStruct in;
        in.bag.setDoubles(kOfxImageEffectPropFrameRange, {a.time, a.time});
        in.bag.setDouble(kOfxImageEffectPropFrameStep, 1.0);
        in.bag.setInt(kOfxPropIsInteractive, 0);
        in.bag.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        in.bag.setInt(kOfxImageEffectPropSequentialRenderStatus, 0);
        in.bag.setInt(kOfxImageEffectPropInteractiveRenderStatus, 0);
        callMain(m_plugin->plugin, kOfxImageEffectActionBeginSequenceRender, &e, &in, nullptr);
    }
    OfxPropertySetStruct in;
    renderArgs(in);
    const OfxStatus s = callMain(m_plugin->plugin, kOfxImageEffectActionRender, &e, &in, nullptr);
    {
        OfxPropertySetStruct end;
        end.bag.setDoubles(kOfxImageEffectPropFrameRange, {a.time, a.time});
        end.bag.setDouble(kOfxImageEffectPropFrameStep, 1.0);
        end.bag.setInt(kOfxPropIsInteractive, 0);
        end.bag.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        end.bag.setInt(kOfxImageEffectPropSequentialRenderStatus, 0);
        end.bag.setInt(kOfxImageEffectPropInteractiveRenderStatus, 0);
        callMain(m_plugin->plugin, kOfxImageEffectActionEndSequenceRender, &e, &end, nullptr);
    }
    for (const auto& clip : e.clips) {
        clip->props.bag.setPointer("wabcut.image", nullptr);
    }
    e.images.clear();
    if (!ok(s)) {
        error = !e.lastMessage.empty() ? e.lastMessage : "render failed (status " + std::to_string(s) + ")";
        return false;
    }
    fromPluginImage(result.pixels, a.width, a.height, depth, rgba, m_outputAlpha, output);
    return true;
}

} // namespace wabcut::ofxhost
