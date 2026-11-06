// OpenFX test plug-ins for the host tests (0.7), written against the raw C API (the vendored OpenFX 1.5 headers), as an
// independent plug-in author would. One source builds three binaries (tests/ofxhost/CMakeLists.txt):
//
//   WabcutTestOfx.ofx           (default) the plug-ins that work:
//     org.wabcut.test.gain      Filter, Float RGBA: Gain (double), Offset (int), Mode (choice), Skip (bool), Tint (RGB), Center
//                               (2D, normalised: a red column is drawn there), Bottom Mark (bool: the bottom row goes green),
//                               Label (string: its length is written into the blue of the bottom-left pixel)
//     org.wabcut.test.invert8   Filter, Byte RGBA: inverts the colour
//     org.wabcut.test.invert16  Filter, Short RGB: inverts the colour (a plug-in that takes no alpha)
//     org.wabcut.test.fail      Filter, Float RGBA: every render fails
//     org.wabcut.test.gpu       Filter: needs OpenGL rendering (listed as unusable)
//     org.wabcut.test.general   General context: takes a second, optional mask clip; halves the colour
//     org.wabcut.test.pair      General context: needs two input clips (listed as unusable)
//   WabcutTestOfxCrash.ofx      org.wabcut.test.crash: crashes (an access violation) when it renders
//   WabcutTestOfxHang.ofx       org.wabcut.test.hang: never returns from render
//   WabcutTestOfxCrashScan.ofx  org.wabcut.test.crashscan: crashes while it is described

#include <openfx/ofxCore.h>
#include <openfx/ofxImageEffect.h>
#include <openfx/ofxMemory.h>
#include <openfx/ofxMessage.h>
#include <openfx/ofxMultiThread.h>
#include <openfx/ofxParam.h>
#include <openfx/ofxProperty.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

OfxHost* gHost = nullptr;
const OfxPropertySuiteV1* gProp = nullptr;
const OfxImageEffectSuiteV1* gEffect = nullptr;
const OfxParameterSuiteV1* gParam = nullptr;
const OfxMultiThreadSuiteV1* gThread = nullptr;
const OfxMessageSuiteV1* gMessage = nullptr;

void fetchSuites() {
    if (gHost == nullptr || gProp != nullptr) {
        return;
    }
    gProp = static_cast<const OfxPropertySuiteV1*>(gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1));
    gEffect = static_cast<const OfxImageEffectSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1));
    gParam = static_cast<const OfxParameterSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1));
    gThread = static_cast<const OfxMultiThreadSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1));
    gMessage = static_cast<const OfxMessageSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1));
}

void setHost(OfxHost* host) { gHost = host; }

// ---- Describing ---------------------------------------------------------------------------------------------------

struct Description {
    const char* label;
    const char* description;
    const char* depth;      ///< the pixel depth it supports
    const char* components; ///< and the components
    const char* context;
    bool needsOpenGl = false;
};

void describeCommon(OfxImageEffectHandle effect, const Description& d) {
    OfxPropertySetHandle props = nullptr;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, d.label);
    gProp->propSetString(props, kOfxPropPluginDescription, 0, d.description);
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Wabcut Tests");
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, d.context);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, d.depth);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gProp->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 0);
    if (d.needsOpenGl) {
        gProp->propSetString(props, "OfxImageEffectPropOpenGLRenderSupported", 0, "needed");
    }
}

void defineClips(OfxImageEffectHandle effect, const char* components, bool mask, bool secondRequired) {
    OfxPropertySetHandle props = nullptr;
    gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &props);
    gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, components);
    gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &props);
    gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, components);
    if (mask) {
        gEffect->clipDefine(effect, "Mask", &props);
        gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentAlpha);
        gProp->propSetInt(props, kOfxImageClipPropOptional, 0, 1);
        gProp->propSetInt(props, kOfxImageClipPropIsMask, 0, 1);
    }
    if (secondRequired) {
        gEffect->clipDefine(effect, "Second", &props);
        gProp->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, components);
        gProp->propSetInt(props, kOfxImageClipPropOptional, 0, 0);
    }
}

OfxPropertySetHandle defineParam(OfxParamSetHandle set, const char* type, const char* name, const char* label, const char* hint = "") {
    OfxPropertySetHandle props = nullptr;
    gParam->paramDefine(set, type, name, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, label);
    gProp->propSetString(props, kOfxParamPropHint, 0, hint);
    return props;
}

void describeGainParams(OfxImageEffectHandle effect) {
    OfxParamSetHandle set = nullptr;
    gEffect->getParamSet(effect, &set);
    OfxPropertySetHandle p = defineParam(set, kOfxParamTypeDouble, "gain", "Gain", "Multiplies the colour");
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 1.0);
    gProp->propSetDouble(p, kOfxParamPropMin, 0, 0.0);
    gProp->propSetDouble(p, kOfxParamPropMax, 0, 4.0);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.0);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, 2.0);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 1);

    p = defineParam(set, kOfxParamTypeInteger, "offset", "Offset (8-bit steps)");
    gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);
    gProp->propSetInt(p, kOfxParamPropMin, 0, 0);
    gProp->propSetInt(p, kOfxParamPropMax, 0, 100);

    p = defineParam(set, kOfxParamTypeChoice, "mode", "Mode");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 0, "Multiply");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 1, "Add");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 2, "Invert");
    gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);

    p = defineParam(set, kOfxParamTypeBoolean, "skip", "Skip");
    gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);

    p = defineParam(set, kOfxParamTypeRGB, "tint", "Tint");
    for (int i = 0; i < 3; ++i) {
        gProp->propSetDouble(p, kOfxParamPropDefault, i, 1.0);
    }

    p = defineParam(set, kOfxParamTypeDouble2D, "center", "Center");
    gProp->propSetString(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
    gProp->propSetString(p, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesNormalised);
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 0.5);
    gProp->propSetDouble(p, kOfxParamPropDefault, 1, 0.5);

    p = defineParam(set, kOfxParamTypeBoolean, "bottomMark", "Bottom Mark");
    gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);

    p = defineParam(set, kOfxParamTypeString, "label", "Label");
    gProp->propSetString(p, kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);
    gProp->propSetString(p, kOfxParamPropDefault, 0, "hello");

    p = defineParam(set, kOfxParamTypeDouble, "hidden", "Hidden");
    gProp->propSetInt(p, kOfxParamPropSecret, 0, 1);

    p = defineParam(set, kOfxParamTypePushButton, "button", "A Button");
    (void)p;

    // A page that puts the parameters in an order of its own (not the declaration order).
    p = defineParam(set, kOfxParamTypePage, "main", "Main");
    const char* order[] = {"label", "center", "tint", "mode", "gain", "offset", "skip", "bottomMark"};
    for (int i = 0; i < 8; ++i) {
        gProp->propSetString(p, kOfxParamPropPageChild, i, order[i]);
    }
    OfxPropertySetHandle effectProps = nullptr;
    gEffect->getPropertySet(effect, &effectProps);
    gProp->propSetString(effectProps, kOfxPluginPropParamPageOrder, 0, "main");
}

// ---- Rendering ----------------------------------------------------------------------------------------------------

struct Image {
    OfxPropertySetHandle handle = nullptr;
    std::uint8_t* data = nullptr;
    int rowBytes = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int channels = 4;
    int bytesPerChannel = 4; ///< 4 Float, 2 Short, 1 Byte

    bool fetch(OfxImageEffectHandle effect, const char* clipName, double time) {
        OfxImageClipHandle clip = nullptr;
        if (gEffect->clipGetHandle(effect, clipName, &clip, nullptr) != kOfxStatOK) {
            return false;
        }
        if (gEffect->clipGetImage(clip, time, nullptr, &handle) != kOfxStatOK || handle == nullptr) {
            return false;
        }
        void* ptr = nullptr;
        gProp->propGetPointer(handle, kOfxImagePropData, 0, &ptr);
        data = static_cast<std::uint8_t*>(ptr);
        gProp->propGetInt(handle, kOfxImagePropRowBytes, 0, &rowBytes);
        gProp->propGetInt(handle, kOfxImagePropBounds, 0, &x1);
        gProp->propGetInt(handle, kOfxImagePropBounds, 1, &y1);
        gProp->propGetInt(handle, kOfxImagePropBounds, 2, &x2);
        gProp->propGetInt(handle, kOfxImagePropBounds, 3, &y2);
        char* comps = nullptr;
        gProp->propGetString(handle, kOfxImageEffectPropComponents, 0, &comps);
        channels = comps != nullptr && std::strcmp(comps, kOfxImageComponentRGB) == 0 ? 3 : 4;
        char* d = nullptr;
        gProp->propGetString(handle, kOfxImageEffectPropPixelDepth, 0, &d);
        bytesPerChannel = d != nullptr && std::strcmp(d, kOfxBitDepthByte) == 0    ? 1
                          : d != nullptr && std::strcmp(d, kOfxBitDepthShort) == 0 ? 2
                                                                                   : 4;
        return data != nullptr;
    }
    std::uint8_t* pixel(int x, int y) const {
        return data + static_cast<std::ptrdiff_t>(y - y1) * rowBytes + static_cast<std::ptrdiff_t>(x - x1) * channels * bytesPerChannel;
    }
    void release() {
        if (handle != nullptr) {
            gEffect->clipReleaseImage(handle);
        }
    }
};

double getDouble(OfxImageEffectHandle effect, const char* name, double time, double fallback) {
    OfxParamSetHandle set = nullptr;
    OfxParamHandle p = nullptr;
    gEffect->getParamSet(effect, &set);
    if (gParam->paramGetHandle(set, name, &p, nullptr) != kOfxStatOK) {
        return fallback;
    }
    double v = fallback;
    gParam->paramGetValueAtTime(p, time, &v);
    return v;
}

int getInt(OfxImageEffectHandle effect, const char* name, double time, int fallback) {
    OfxParamSetHandle set = nullptr;
    OfxParamHandle p = nullptr;
    gEffect->getParamSet(effect, &set);
    if (gParam->paramGetHandle(set, name, &p, nullptr) != kOfxStatOK) {
        return fallback;
    }
    int v = fallback;
    gParam->paramGetValueAtTime(p, time, &v);
    return v;
}

void renderWindow(OfxPropertySetHandle in, int& x1, int& y1, int& x2, int& y2) {
    gProp->propGetInt(in, kOfxImageEffectPropRenderWindow, 0, &x1);
    gProp->propGetInt(in, kOfxImageEffectPropRenderWindow, 1, &y1);
    gProp->propGetInt(in, kOfxImageEffectPropRenderWindow, 2, &x2);
    gProp->propGetInt(in, kOfxImageEffectPropRenderWindow, 3, &y2);
}

struct GainArgs {
    Image* src;
    Image* dst;
    double gain, tint[3];
    int offset, mode, markX;
    bool bottomMark;
    int labelLength;
    int x1, y1, x2, y2;
};

void gainSlice(unsigned int index, unsigned int max, void* arg) {
    const auto* a = static_cast<const GainArgs*>(arg);
    const int rows = a->y2 - a->y1;
    const int begin = a->y1 + static_cast<int>(static_cast<long long>(rows) * index / max);
    const int end = a->y1 + static_cast<int>(static_cast<long long>(rows) * (index + 1) / max);
    for (int y = begin; y < end; ++y) {
        for (int x = a->x1; x < a->x2; ++x) {
            const auto* s = reinterpret_cast<const float*>(a->src->pixel(x, y));
            auto* d = reinterpret_cast<float*>(a->dst->pixel(x, y));
            const float alpha = a->src->channels == 4 ? s[3] : 1.0f;
            for (int c = 0; c < 3; ++c) {
                float v = s[c];
                if (a->mode == 0) {
                    v = static_cast<float>(v * a->gain * a->tint[c]);
                } else if (a->mode == 1) {
                    v = v + static_cast<float>(a->offset) / 255.0f;
                } else {
                    v = alpha - v;
                }
                d[c] = v;
            }
            if (a->dst->channels == 4) {
                d[3] = alpha;
            }
            if (x == a->markX) {
                d[0] = 1.0f;
                d[1] = 0.0f;
                d[2] = 0.0f;
            }
            if (a->bottomMark && y == a->dst->y1) {
                d[0] = 0.0f;
                d[1] = 1.0f;
                d[2] = 0.0f;
            }
            if (a->labelLength > 0 && x == a->dst->x1 && y == a->dst->y1) {
                d[2] = static_cast<float>(a->labelLength % 256) / 255.0f;
            }
        }
    }
}

OfxStatus renderGain(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    double time = 0.0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    renderWindow(in, x1, y1, x2, y2);
    Image src;
    Image dst;
    if (!src.fetch(effect, kOfxImageEffectSimpleSourceClipName, time) || !dst.fetch(effect, kOfxImageEffectOutputClipName, time)) {
        return kOfxStatFailed;
    }
    GainArgs a{};
    a.src = &src;
    a.dst = &dst;
    a.gain = getDouble(effect, "gain", time, 1.0);
    a.offset = getInt(effect, "offset", time, 0);
    a.mode = getInt(effect, "mode", time, 0);
    a.bottomMark = getInt(effect, "bottomMark", time, 0) != 0;
    OfxParamSetHandle set = nullptr;
    OfxParamHandle p = nullptr;
    gEffect->getParamSet(effect, &set);
    a.tint[0] = a.tint[1] = a.tint[2] = 1.0;
    if (gParam->paramGetHandle(set, "tint", &p, nullptr) == kOfxStatOK) {
        gParam->paramGetValueAtTime(p, time, &a.tint[0], &a.tint[1], &a.tint[2]);
    }
    double cx = 0.5, cy = 0.5;
    if (gParam->paramGetHandle(set, "center", &p, nullptr) == kOfxStatOK) {
        gParam->paramGetValueAtTime(p, time, &cx, &cy);
    }
    a.markX = static_cast<int>(std::lround(cx));
    char* label = nullptr;
    if (gParam->paramGetHandle(set, "label", &p, nullptr) == kOfxStatOK && gParam->paramGetValueAtTime(p, time, &label) == kOfxStatOK && label != nullptr) {
        a.labelLength = static_cast<int>(std::strlen(label));
    }
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    unsigned int threads = 1;
    gThread->multiThreadNumCPUs(&threads);
    gThread->multiThread(gainSlice, std::min(threads, 4u), &a);
    src.release();
    dst.release();
    return kOfxStatOK;
}

OfxStatus isIdentityGain(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    double time = 0.0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    if (getInt(effect, "skip", time, 0) != 0) {
        gProp->propSetString(out, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

/// Inverts the colour of an 8-bit RGBA or 16-bit RGB image (the plug-in takes only that depth).
OfxStatus renderInvert(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    double time = 0.0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    renderWindow(in, x1, y1, x2, y2);
    Image src;
    Image dst;
    if (!src.fetch(effect, kOfxImageEffectSimpleSourceClipName, time) || !dst.fetch(effect, kOfxImageEffectOutputClipName, time)) {
        return kOfxStatFailed;
    }
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            const std::uint8_t* s = src.pixel(x, y);
            std::uint8_t* d = dst.pixel(x, y);
            if (src.bytesPerChannel == 1) {
                d[0] = static_cast<std::uint8_t>(255 - s[0]);
                d[1] = static_cast<std::uint8_t>(255 - s[1]);
                d[2] = static_cast<std::uint8_t>(255 - s[2]);
                if (dst.channels == 4) {
                    d[3] = s[3];
                }
            } else {
                for (int c = 0; c < 3; ++c) {
                    std::uint16_t v = 0;
                    std::memcpy(&v, s + c * 2, 2);
                    v = static_cast<std::uint16_t>(65535 - v);
                    std::memcpy(d + c * 2, &v, 2);
                }
            }
        }
    }
    src.release();
    dst.release();
    return kOfxStatOK;
}

OfxStatus renderHalve(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    double time = 0.0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &time);
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    renderWindow(in, x1, y1, x2, y2);
    Image src;
    Image dst;
    if (!src.fetch(effect, kOfxImageEffectSimpleSourceClipName, time) || !dst.fetch(effect, kOfxImageEffectOutputClipName, time)) {
        return kOfxStatFailed;
    }
    // The optional mask clip is not connected in this host: asking for its image fails, and the plug-in carries on.
    OfxImageClipHandle mask = nullptr;
    OfxPropertySetHandle maskImage = nullptr;
    if (gEffect->clipGetHandle(effect, "Mask", &mask, nullptr) == kOfxStatOK) {
        if (gEffect->clipGetImage(mask, time, nullptr, &maskImage) == kOfxStatOK && maskImage != nullptr) {
            gEffect->clipReleaseImage(maskImage);
        }
    }
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            const auto* s = reinterpret_cast<const float*>(src.pixel(x, y));
            auto* d = reinterpret_cast<float*>(dst.pixel(x, y));
            d[0] = s[0] * 0.5f;
            d[1] = s[1] * 0.5f;
            d[2] = s[2] * 0.5f;
            d[3] = src.channels == 4 ? s[3] : 1.0f;
        }
    }
    src.release();
    dst.release();
    return kOfxStatOK;
}

// ---- The plug-ins' entry points -----------------------------------------------------------------------------------

enum class Kind { Gain, Invert8, Invert16, Fail, Gpu, General, Pair, Crash, Hang, CrashScan };

OfxStatus entry(Kind kind, const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    const auto effect = reinterpret_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    const std::string a = action;
    if (a == kOfxActionLoad) {
        fetchSuites();
        return gProp != nullptr && gEffect != nullptr && gParam != nullptr ? kOfxStatOK : kOfxStatErrMissingHostFeature;
    }
    if (a == kOfxActionUnload) {
        return kOfxStatOK;
    }
    if (a == kOfxActionDescribe) {
        if (kind == Kind::CrashScan) {
            *static_cast<volatile int*>(nullptr) = 1; // crashes while it is described
        }
        switch (kind) {
        case Kind::Gain:
            describeCommon(effect, {"Wabcut Test Gain", "Gain, offset, invert and marks, for the host's tests", kOfxBitDepthFloat, kOfxImageComponentRGBA, kOfxImageEffectContextFilter});
            break;
        case Kind::Invert8:
            describeCommon(effect, {"Wabcut Test Invert 8-bit", "Inverts an 8-bit picture", kOfxBitDepthByte, kOfxImageComponentRGBA, kOfxImageEffectContextFilter});
            break;
        case Kind::Invert16:
            describeCommon(effect, {"Wabcut Test Invert 16-bit RGB", "Inverts a 16-bit RGB picture", kOfxBitDepthShort, kOfxImageComponentRGB, kOfxImageEffectContextFilter});
            break;
        case Kind::Fail:
            describeCommon(effect, {"Wabcut Test Fail", "Fails every render", kOfxBitDepthFloat, kOfxImageComponentRGBA, kOfxImageEffectContextFilter});
            break;
        case Kind::Gpu:
            describeCommon(effect, {"Wabcut Test GPU", "Needs OpenGL", kOfxBitDepthFloat, kOfxImageComponentRGBA, kOfxImageEffectContextFilter, true});
            break;
        case Kind::General:
            describeCommon(effect, {"Wabcut Test General", "A general-context effect with an optional mask", kOfxBitDepthFloat, kOfxImageComponentRGBA, kOfxImageEffectContextGeneral});
            break;
        case Kind::Pair:
            describeCommon(effect, {"Wabcut Test Pair", "Needs two inputs", kOfxBitDepthFloat, kOfxImageComponentRGBA, kOfxImageEffectContextGeneral});
            break;
        case Kind::Crash:
            describeCommon(effect, {"Wabcut Test Crash", "Crashes when it renders", kOfxBitDepthFloat, kOfxImageComponentRGBA, kOfxImageEffectContextFilter});
            break;
        case Kind::Hang:
            describeCommon(effect, {"Wabcut Test Hang", "Never returns from a render", kOfxBitDepthFloat, kOfxImageComponentRGBA, kOfxImageEffectContextFilter});
            break;
        case Kind::CrashScan:
            break;
        }
        return kOfxStatOK;
    }
    if (a == kOfxImageEffectActionDescribeInContext) {
        (void)in;
        switch (kind) {
        case Kind::Gain:
            defineClips(effect, kOfxImageComponentRGBA, false, false);
            describeGainParams(effect);
            break;
        case Kind::Invert16:
            defineClips(effect, kOfxImageComponentRGB, false, false);
            break;
        case Kind::General:
            defineClips(effect, kOfxImageComponentRGBA, true, false);
            break;
        case Kind::Pair:
            defineClips(effect, kOfxImageComponentRGBA, false, true);
            break;
        default:
            defineClips(effect, kOfxImageComponentRGBA, false, false);
            break;
        }
        return kOfxStatOK;
    }
    if (a == kOfxActionCreateInstance || a == kOfxActionDestroyInstance || a == kOfxActionBeginInstanceChanged ||
        a == kOfxActionInstanceChanged || a == kOfxActionEndInstanceChanged) {
        return kOfxStatOK;
    }
    if (a == kOfxImageEffectActionIsIdentity && kind == Kind::Gain) {
        return isIdentityGain(effect, in, out);
    }
    if (a == kOfxImageEffectActionGetClipPreferences) {
        if (kind == Kind::Gain && out != nullptr) {
            gProp->propSetString(out, kOfxImageEffectPropPreMultiplication, 0, kOfxImagePreMultiplied);
            return kOfxStatOK;
        }
        return kOfxStatReplyDefault;
    }
    if (a == kOfxImageEffectActionRender) {
        switch (kind) {
        case Kind::Gain:
            return renderGain(effect, in);
        case Kind::Invert8:
        case Kind::Invert16:
            return renderInvert(effect, in);
        case Kind::General:
            return renderHalve(effect, in);
        case Kind::Fail:
            if (gMessage != nullptr) {
                gMessage->message(effect, kOfxMessageError, "", "this plug-in always fails");
            }
            return kOfxStatFailed;
        case Kind::Crash:
            *static_cast<volatile int*>(nullptr) = 1;
            return kOfxStatOK;
        case Kind::Hang:
            for (;;) {
                Sleep(1000);
            }
        default:
            return kOfxStatFailed;
        }
    }
    if (a == kOfxImageEffectActionBeginSequenceRender || a == kOfxImageEffectActionEndSequenceRender) {
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

template <Kind K>
OfxStatus entryFor(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    return entry(K, action, handle, in, out);
}

struct Table {
    OfxPlugin plugins[8];
    int count = 0;
    void add(const char* id, OfxPluginEntryPoint* main) {
        plugins[count++] = OfxPlugin{kOfxImageEffectPluginApi, 1, id, 1, 3, setHost, main};
    }
    Table() {
#if defined(WABCUT_TEST_OFX_CRASH)
        add("org.wabcut.test.crash", entryFor<Kind::Crash>);
#elif defined(WABCUT_TEST_OFX_HANG)
        add("org.wabcut.test.hang", entryFor<Kind::Hang>);
#elif defined(WABCUT_TEST_OFX_CRASH_ON_SCAN)
        add("org.wabcut.test.crashscan", entryFor<Kind::CrashScan>);
#else
        add("org.wabcut.test.gain", entryFor<Kind::Gain>);
        add("org.wabcut.test.invert8", entryFor<Kind::Invert8>);
        add("org.wabcut.test.invert16", entryFor<Kind::Invert16>);
        add("org.wabcut.test.fail", entryFor<Kind::Fail>);
        add("org.wabcut.test.gpu", entryFor<Kind::Gpu>);
        add("org.wabcut.test.general", entryFor<Kind::General>);
        add("org.wabcut.test.pair", entryFor<Kind::Pair>);
#endif
    }
};

Table& table() {
    static Table t;
    return t;
}

} // namespace

OfxExport int OfxGetNumberOfPlugins(void) { return table().count; }

OfxExport OfxPlugin* OfxGetPlugin(int nth) { return nth >= 0 && nth < table().count ? &table().plugins[nth] : nullptr; }
