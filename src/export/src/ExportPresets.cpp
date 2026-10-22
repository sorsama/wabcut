#include <wabcut/export/ExportPresets.h>

#include "Extras.h"

#include <wabcut/export/Encoders.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace wabcut::exporter {

namespace {

using Result = std::expected<void, Error>;

Error bad(const std::string& path, std::string_view problem) {
    return Error{ErrorCode::InvalidArgument, std::format("{}: {}", path, problem)};
}

std::string join(const std::string& path, std::string_view key) {
    return path.empty() ? std::string(key) : path + "." + std::string(key);
}

const Json* member(const Json& obj, std::string_view key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.find(std::string(key));
    return it == obj.end() || it->is_null() ? nullptr : &*it;
}

Result getBool(const Json& obj, std::string_view key, bool& out, const std::string& path) {
    if (const Json* v = member(obj, key)) {
        if (!v->is_boolean()) {
            return std::unexpected(bad(join(path, key), "must be true or false"));
        }
        out = v->get<bool>();
    }
    return {};
}

Result getInt(const Json& obj, std::string_view key, std::int64_t& out, std::int64_t lo, std::int64_t hi,
              const std::string& path) {
    if (const Json* v = member(obj, key)) {
        if (!v->is_number()) {
            return std::unexpected(bad(join(path, key), "must be a number"));
        }
        const double d = v->get<double>();
        std::int64_t i = 0;
        if (v->is_number_integer()) {
            i = v->is_number_unsigned() ? static_cast<std::int64_t>(std::min<std::uint64_t>(v->get<std::uint64_t>(), INT64_MAX))
                                        : v->get<std::int64_t>();
        } else if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9.0e18) {
            i = static_cast<std::int64_t>(d);
        } else {
            return std::unexpected(bad(join(path, key), "must be a whole number"));
        }
        if (i < lo || i > hi) {
            return std::unexpected(bad(join(path, key), std::format("must be {}..{}", lo, hi)));
        }
        out = i;
    }
    return {};
}

template <class T>
Result getIntAs(const Json& obj, std::string_view key, T& out, std::int64_t lo, std::int64_t hi, const std::string& path) {
    std::int64_t v = static_cast<std::int64_t>(out);
    if (auto ok = getInt(obj, key, v, lo, hi, path); !ok) {
        return ok;
    }
    out = static_cast<T>(v);
    return {};
}

Result getDouble(const Json& obj, std::string_view key, double& out, double lo, double hi, const std::string& path) {
    if (const Json* v = member(obj, key)) {
        if (!v->is_number()) {
            return std::unexpected(bad(join(path, key), "must be a number"));
        }
        const double d = v->get<double>();
        if (!std::isfinite(d) || d < lo || d > hi) {
            return std::unexpected(bad(join(path, key), std::format("must be {}..{}", lo, hi)));
        }
        out = d;
    }
    return {};
}

Result getString(const Json& obj, std::string_view key, std::string& out, std::size_t maxBytes, const std::string& path) {
    if (const Json* v = member(obj, key)) {
        if (!v->is_string()) {
            return std::unexpected(bad(join(path, key), "must be text"));
        }
        std::string s = v->get<std::string>();
        if (s.size() > maxBytes || s.find('\0') != std::string::npos) {
            return std::unexpected(bad(join(path, key), std::format("is at most {} bytes", maxBytes)));
        }
        out = std::move(s);
    }
    return {};
}

template <class E, class FromName>
Result getEnum(const Json& obj, std::string_view key, E& out, FromName fromName, const std::string& path) {
    if (const Json* v = member(obj, key)) {
        if (!v->is_string()) {
            return std::unexpected(bad(join(path, key), "must be text"));
        }
        const auto e = fromName(v->get<std::string>());
        if (!e) {
            return std::unexpected(bad(join(path, key), std::format("\"{}\" is not one of its values", v->get<std::string>())));
        }
        out = *e;
    }
    return {};
}

#define WABCUT_TRY(expr)                                                                                               \
    do {                                                                                                               \
        if (auto ok_ = (expr); !ok_) {                                                                                 \
            return std::unexpected(ok_.error());                                                                       \
        }                                                                                                              \
    } while (false)

std::string_view rateControlName(RateControl r) {
    switch (r) {
    case RateControl::Quality:
        return "quality";
    case RateControl::Bitrate:
        return "bitrate";
    case RateControl::TargetSize:
        return "targetSize";
    }
    return "quality";
}

std::optional<RateControl> rateControlFromName(std::string_view n) {
    for (int r = 0; r <= static_cast<int>(RateControl::TargetSize); ++r) {
        if (rateControlName(static_cast<RateControl>(r)) == n) {
            return static_cast<RateControl>(r);
        }
    }
    return std::nullopt;
}

std::string_view scaleModeName(ScaleMode m) {
    switch (m) {
    case ScaleMode::Fit:
        return "fit";
    case ScaleMode::Fill:
        return "fill";
    case ScaleMode::Stretch:
        return "stretch";
    }
    return "fit";
}

std::optional<ScaleMode> scaleModeFromName(std::string_view n) {
    for (int m = 0; m <= static_cast<int>(ScaleMode::Stretch); ++m) {
        if (scaleModeName(static_cast<ScaleMode>(m)) == n) {
            return static_cast<ScaleMode>(m);
        }
    }
    return std::nullopt;
}

std::optional<OutputPixelFormat> pixelFormatFromName(std::string_view n) {
    for (int f = 0; f <= static_cast<int>(OutputPixelFormat::Rgbaf); ++f) {
        if (pixelFormatName(static_cast<OutputPixelFormat>(f)) == n) {
            return static_cast<OutputPixelFormat>(f);
        }
    }
    return std::nullopt;
}

std::optional<ProResProfile> proresProfileFromName(std::string_view n) {
    for (int p = 0; p <= static_cast<int>(ProResProfile::P4444Xq); ++p) {
        if (proresProfileName(static_cast<ProResProfile>(p)) == n) {
            return static_cast<ProResProfile>(p);
        }
    }
    return std::nullopt;
}

std::optional<DnxhrProfile> dnxhrProfileFromName(std::string_view n) {
    for (int p = 0; p <= static_cast<int>(DnxhrProfile::DnxhdHqx); ++p) {
        if (dnxhrProfileName(static_cast<DnxhrProfile>(p)) == n) {
            return static_cast<DnxhrProfile>(p);
        }
    }
    return std::nullopt;
}

std::optional<DynamicRange> dynamicRangeFromName(std::string_view n) {
    for (int d = 0; d <= static_cast<int>(DynamicRange::Hlg); ++d) {
        if (dynamicRangeName(static_cast<DynamicRange>(d)) == n) {
            return static_cast<DynamicRange>(d);
        }
    }
    return std::nullopt;
}

/// Caption modes by name, also the ones later builds add (captionExportName answers "none" for values it does not
/// know).
std::optional<CaptionExport> captionModeFromName(std::string_view n) {
    if (n == "none") {
        return CaptionExport::None;
    }
    for (int m = 1; m < 64; ++m) {
        if (captionExportName(static_cast<CaptionExport>(m)) == n) {
            return static_cast<CaptionExport>(m);
        }
    }
    return std::nullopt;
}

std::optional<TimecodeSource> timecodeSourceFromName(std::string_view n) {
    if (n == "sequence") {
        return TimecodeSource::Sequence;
    }
    if (n == "generate") {
        return TimecodeSource::Generate;
    }
    return std::nullopt;
}

std::optional<TimeDisplay> timeDisplayFromName(std::string_view n) {
    if (n == "timecode") {
        return TimeDisplay::Timecode;
    }
    if (n == "frames") {
        return TimeDisplay::Frames;
    }
    return std::nullopt;
}

Json placementJson(const OverlayPlacement& p) {
    return Json{{"position", overlayPositionName(p.position)},
                {"offsetX", p.offsetX},
                {"offsetY", p.offsetY},
                {"size", p.size},
                {"opacity", p.opacity}};
}

Result readPlacement(const Json& j, OverlayPlacement& p, const std::string& path) {
    WABCUT_TRY(getEnum(j, "position", p.position, overlayPositionFromName, path));
    WABCUT_TRY(getIntAs(j, "offsetX", p.offsetX, -4096, 4096, path));
    WABCUT_TRY(getIntAs(j, "offsetY", p.offsetY, -4096, 4096, path));
    WABCUT_TRY(getDouble(j, "size", p.size, 1.0, 1000.0, path));
    WABCUT_TRY(getDouble(j, "opacity", p.opacity, 0.0, 100.0, path));
    return {};
}

Result requireObject(const Json& j, const std::string& path) {
    if (!j.is_object()) {
        return std::unexpected(bad(path, "must be an object"));
    }
    return {};
}

Json effectsJson(const ExportEffects& fx) {
    Json image = placementJson(fx.image.placement);
    image["applied"] = fx.image.applied;
    image["image"] = fx.image.image;
    Json name = placementJson(fx.name.placement);
    name["applied"] = fx.name.applied;
    name["prefix"] = fx.name.prefix;
    name["suffix"] = fx.name.suffix;
    name["format"] = nameOverlayFormatName(fx.name.format);
    Json tc = placementJson(fx.timecode.placement);
    tc["applied"] = fx.timecode.applied;
    tc["source"] = fx.timecode.source == TimecodeSource::Generate ? "generate" : "sequence";
    tc["startTimecode"] = fx.timecode.startTimecode.value;
    tc["display"] = fx.timecode.display == TimeDisplay::Frames ? "frames" : "timecode";
    return Json{{"look", {{"applied", fx.look.applied}, {"lut", fx.look.lut}}},
                {"limiter",
                 {{"applied", fx.limiter.applied}, {"clipLevel", fx.limiter.clipLevel}, {"compression", fx.limiter.compression}}},
                {"imageOverlay", std::move(image)},
                {"nameOverlay", std::move(name)},
                {"timecodeOverlay", std::move(tc)},
                {"loudness",
                 {{"applied", fx.loudness.applied},
                  {"standard", loudnessStandardName(fx.loudness.standard)},
                  {"target", fx.loudness.targetLufs},
                  {"tolerance", fx.loudness.toleranceLu},
                  {"maxTruePeak", fx.loudness.maxTruePeakDbtp},
                  {"writeLog", fx.loudness.writeLog}}}};
}

Result readEffects(const Json& j, ExportEffects& fx, const std::string& path) {
    WABCUT_TRY(requireObject(j, path));
    if (const Json* look = member(j, "look")) {
        const std::string p = join(path, "look");
        WABCUT_TRY(requireObject(*look, p));
        WABCUT_TRY(getBool(*look, "applied", fx.look.applied, p));
        WABCUT_TRY(getString(*look, "lut", fx.look.lut, kMaxExportPathBytes, p));
    }
    if (const Json* lim = member(j, "limiter")) {
        const std::string p = join(path, "limiter");
        WABCUT_TRY(requireObject(*lim, p));
        WABCUT_TRY(getBool(*lim, "applied", fx.limiter.applied, p));
        WABCUT_TRY(getDouble(*lim, "clipLevel", fx.limiter.clipLevel, 90.0, 109.0, p));
        WABCUT_TRY(getDouble(*lim, "compression", fx.limiter.compression, 0.0, 20.0, p));
    }
    if (const Json* img = member(j, "imageOverlay")) {
        const std::string p = join(path, "imageOverlay");
        WABCUT_TRY(requireObject(*img, p));
        WABCUT_TRY(getBool(*img, "applied", fx.image.applied, p));
        WABCUT_TRY(getString(*img, "image", fx.image.image, kMaxExportPathBytes, p));
        WABCUT_TRY(readPlacement(*img, fx.image.placement, p));
    }
    if (const Json* name = member(j, "nameOverlay")) {
        const std::string p = join(path, "nameOverlay");
        WABCUT_TRY(requireObject(*name, p));
        WABCUT_TRY(getBool(*name, "applied", fx.name.applied, p));
        WABCUT_TRY(getString(*name, "prefix", fx.name.prefix, 256, p));
        WABCUT_TRY(getString(*name, "suffix", fx.name.suffix, 256, p));
        WABCUT_TRY(getEnum(*name, "format", fx.name.format, nameOverlayFormatFromName, p));
        WABCUT_TRY(readPlacement(*name, fx.name.placement, p));
    }
    if (const Json* tc = member(j, "timecodeOverlay")) {
        const std::string p = join(path, "timecodeOverlay");
        WABCUT_TRY(requireObject(*tc, p));
        WABCUT_TRY(getBool(*tc, "applied", fx.timecode.applied, p));
        WABCUT_TRY(getEnum(*tc, "source", fx.timecode.source, timecodeSourceFromName, p));
        WABCUT_TRY(getIntAs(*tc, "startTimecode", fx.timecode.startTimecode.value, 0, kMaxStartTimecode.value, p));
        WABCUT_TRY(getEnum(*tc, "display", fx.timecode.display, timeDisplayFromName, p));
        WABCUT_TRY(readPlacement(*tc, fx.timecode.placement, p));
    }
    if (const Json* ln = member(j, "loudness")) {
        const std::string p = join(path, "loudness");
        WABCUT_TRY(requireObject(*ln, p));
        WABCUT_TRY(getBool(*ln, "applied", fx.loudness.applied, p));
        WABCUT_TRY(getEnum(*ln, "standard", fx.loudness.standard, loudnessStandardFromName, p));
        WABCUT_TRY(getDouble(*ln, "target", fx.loudness.targetLufs, -40.0, -5.0, p));
        WABCUT_TRY(getDouble(*ln, "tolerance", fx.loudness.toleranceLu, 0.0, 10.0, p));
        WABCUT_TRY(getDouble(*ln, "maxTruePeak", fx.loudness.maxTruePeakDbtp, -12.0, 0.0, p));
        WABCUT_TRY(getBool(*ln, "writeLog", fx.loudness.writeLog, p));
    }
    return {};
}

Json metadataJson(const ExportMetadata& m) {
    return Json{{"chapters", m.chapters},
                {"timecode", m.timecode},
                {"startTimecode", m.startTimecode ? Json(m.startTimecode->value) : Json(nullptr)},
                {"xmp", xmpModeName(m.xmp)},
                {"title", m.title},
                {"description", m.description},
                {"xmpMarkers", m.xmpMarkers}};
}

Result readMetadata(const Json& j, ExportMetadata& m, const std::string& path) {
    WABCUT_TRY(requireObject(j, path));
    WABCUT_TRY(getBool(j, "chapters", m.chapters, path));
    WABCUT_TRY(getBool(j, "timecode", m.timecode, path));
    if (j.contains("startTimecode") && j.at("startTimecode").is_null()) {
        m.startTimecode.reset();
    } else if (member(j, "startTimecode")) {
        Ticks t;
        WABCUT_TRY(getIntAs(j, "startTimecode", t.value, 0, kMaxStartTimecode.value, path));
        m.startTimecode = t;
    }
    WABCUT_TRY(getEnum(j, "xmp", m.xmp, xmpModeFromName, path));
    WABCUT_TRY(getString(j, "title", m.title, 1024, path));
    WABCUT_TRY(getString(j, "description", m.description, 4096, path));
    WABCUT_TRY(getBool(j, "xmpMarkers", m.xmpMarkers, path));
    return {};
}

Json renderJson(const ExportRenderOptions& r) {
    return Json{{"maximumRenderQuality", r.maximumRenderQuality},
                {"maximumDepth", r.maximumDepth},
                {"usePreviews", r.usePreviews},
                {"smartRender", r.smartRender}};
}

Result readRender(const Json& j, ExportRenderOptions& r, const std::string& path) {
    WABCUT_TRY(requireObject(j, path));
    WABCUT_TRY(getBool(j, "maximumRenderQuality", r.maximumRenderQuality, path));
    WABCUT_TRY(getBool(j, "maximumDepth", r.maximumDepth, path));
    WABCUT_TRY(getBool(j, "usePreviews", r.usePreviews, path));
    WABCUT_TRY(getBool(j, "smartRender", r.smartRender, path));
    return {};
}

Result readSubtitles(const Json& j, SubtitleTrackExport& s, const std::string& path) {
    WABCUT_TRY(requireObject(j, path));
    WABCUT_TRY(getBool(j, "enabled", s.enabled, path));
    WABCUT_TRY(getString(j, "language", s.language, 3, path));
    return {};
}

// ---- Override values (QVariant-like: numbers may come as numeric text, booleans as numbers) -----------------------

std::optional<double> numberOf(const Json& v) {
    if (v.is_number()) {
        const double d = v.get<double>();
        return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        try {
            std::size_t used = 0;
            const double d = std::stod(s, &used);
            if (used == s.size() && std::isfinite(d)) {
                return d;
            }
        } catch (...) {
        }
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? 1.0 : 0.0;
    }
    return std::nullopt;
}

bool boolOf(const Json& v) {
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    if (v.is_string()) {
        return v.get<std::string>() == "true";
    }
    if (const auto d = numberOf(v)) {
        return *d != 0.0;
    }
    return false;
}

std::string textOf(const Json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer()) {
        return std::to_string(v.get<std::int64_t>());
    }
    if (v.is_number()) {
        return std::format("{}", v.get<double>());
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    return v.dump();
}

Error badOverride(std::string_view key, const Json& value) {
    return Error{ErrorCode::InvalidArgument, std::format("Unsupported export option {} = {}", key, textOf(value))};
}

/// "60", "59.94", "60000/1001" (NTSC rates by their usual decimals); 0 = the sequence's.
std::optional<std::optional<Rate>> parseFrameRate(const Json& value) {
    std::string text = textOf(value);
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    if (const auto slash = text.find('/'); slash != std::string::npos && slash > 0) {
        try {
            std::size_t a = 0;
            std::size_t b = 0;
            const long long num = std::stoll(text.substr(0, slash), &a);
            const long long den = std::stoll(text.substr(slash + 1), &b);
            if (a != slash || b != text.size() - slash - 1 || num <= 0 || den <= 0 || num / den > 1000 ||
                num > INT32_MAX || den > INT32_MAX) {
                return std::nullopt;
            }
            return std::optional<Rate>(Rate{static_cast<std::int32_t>(num), static_cast<std::int32_t>(den)}.normalized());
        } catch (...) {
            return std::nullopt;
        }
    }
    const auto fps = numberOf(value);
    if (!fps || *fps < 0 || *fps > 1000) {
        return std::nullopt;
    }
    if (*fps == 0) {
        return std::optional<Rate>(); // the sequence rate
    }
    for (const int base : {24, 30, 60, 120, 240}) { // 23.976, 29.97, 59.94, 119.88, 239.76
        if (std::abs(*fps - base * 1000.0 / 1001.0) < 0.01) {
            return std::optional<Rate>(Rate{base * 1000, 1001});
        }
    }
    if (std::abs(*fps - std::round(*fps)) > 1e-6) {
        return std::nullopt;
    }
    return std::optional<Rate>(Rate{static_cast<std::int32_t>(std::lround(*fps)), 1});
}

/// The pixel format a codec writes by default, with or without alpha.
OutputPixelFormat defaultPixelFormat(VideoCodec codec, bool alpha, const VideoSettings& v) {
    using F = OutputPixelFormat;
    switch (codec) {
    case VideoCodec::ProRes:
        return alpha ? F::Yuva444p10
               : (v.proresProfile == ProResProfile::P4444 || v.proresProfile == ProResProfile::P4444Xq) ? F::Yuv444p10
                                                                                                         : F::Yuv422p10;
    case VideoCodec::DnxHR:
        return v.dnxhrProfile == DnxhrProfile::Hqx || v.dnxhrProfile == DnxhrProfile::DnxhdHqx ? F::Yuv422p10
               : v.dnxhrProfile == DnxhrProfile::Hr444                                         ? F::Yuv444p10
                                                                                               : F::Yuv422p;
    case VideoCodec::Gif:
        return F::Pal8;
    case VideoCodec::Png:
    case VideoCodec::Tiff:
    case VideoCodec::Targa:
    case VideoCodec::Animation:
        return alpha ? F::Rgba : F::Rgb24;
    case VideoCodec::Dpx:
        return F::Gbrp10;
    case VideoCodec::Jpeg:
        return F::Rgb24;
    case VideoCodec::Exr:
        return alpha ? F::Rgbaf : F::Rgbf;
    case VideoCodec::CineForm:
        return alpha ? F::Gbrap12 : F::Yuv422p10;
    default:
        return F::Yuv420p;
    }
}

} // namespace

std::string_view proresProfileName(ProResProfile p) {
    switch (p) {
    case ProResProfile::Proxy:
        return "proxy";
    case ProResProfile::Lt:
        return "lt";
    case ProResProfile::Standard:
        return "standard";
    case ProResProfile::Hq:
        return "hq";
    case ProResProfile::P4444:
        return "4444";
    case ProResProfile::P4444Xq:
        return "4444xq";
    }
    return "hq";
}

std::string_view dnxhrProfileName(DnxhrProfile p) {
    switch (p) {
    case DnxhrProfile::Lb:
        return "lb";
    case DnxhrProfile::Sq:
        return "sq";
    case DnxhrProfile::Hq:
        return "hq";
    case DnxhrProfile::Hqx:
        return "hqx";
    case DnxhrProfile::Hr444:
        return "444";
    case DnxhrProfile::DnxhdLb:
        return "dnxhd-lb";
    case DnxhrProfile::DnxhdSq:
        return "dnxhd-sq";
    case DnxhrProfile::DnxhdHq:
        return "dnxhd-hq";
    case DnxhrProfile::DnxhdHqx:
        return "dnxhd-hqx";
    }
    return "hq";
}

Json settingsToJson(const ExportSettings& s) {
    const VideoSettings& v = s.video;
    Json video{{"enabled", v.enabled},
               {"codec", videoCodecName(v.codec)},
               {"encoder", v.encoder},
               {"rateControl", rateControlName(v.rateControl)},
               {"quality", v.quality},
               {"bitrate", v.bitrate},
               {"maxBitrate", v.maxBitrate},
               {"targetBytes", v.targetBytes},
               {"width", v.width ? Json(*v.width) : Json(nullptr)},
               {"height", v.height ? Json(*v.height) : Json(nullptr)},
               {"scaleMode", scaleModeName(v.scaleMode)},
               {"frameRate", v.frameRate ? Json{{"num", v.frameRate->num}, {"den", v.frameRate->den}} : Json(nullptr)},
               {"pixelFormat", pixelFormatName(v.pixelFormat)},
               {"proresProfile", proresProfileName(v.proresProfile)},
               {"dnxhrProfile", dnxhrProfileName(v.dnxhrProfile)},
               {"keyframeInterval", v.keyframeInterval.value},
               {"dynamicRange", dynamicRangeName(v.dynamicRange)},
               {"exportAsSequence", v.exportAsSequence}};
    const AudioSettings& a = s.audio;
    Json audio{{"enabled", a.enabled},
               {"codec", audioCodecKey(a.codec)},
               {"sampleRate", a.sampleRate},
               {"channels", a.channels},
               {"bitrate", a.bitrate}};
    Json captions{{"mode", captionExportName(s.captions.mode)},
                  {"track", s.captions.track ? Json(*s.captions.track) : Json(nullptr)},
                  {"karaoke", s.captions.karaoke},
                  {"redactedWord", s.captions.redactedWord}};
    return Json{{"presetId", s.presetId},
                {"container", containerName(s.container)},
                {"video", std::move(video)},
                {"audio", std::move(audio)},
                {"range", rangeModeName(s.range)},
                {"customRange", s.customRange ? Json{{"start", s.customRange->start.value}, {"duration", s.customRange->duration.value}}
                                              : Json(nullptr)},
                {"captions", std::move(captions)},
                {"effects", effectsJson(s.effects)},
                {"metadata", metadataJson(s.metadata)},
                {"subtitles", {{"enabled", s.subtitles.enabled}, {"language", s.subtitles.language}}},
                {"render", renderJson(s.render)}};
}

std::expected<ExportSettings, Error> settingsFromJson(const Json& j) {
    if (!j.is_object()) {
        return std::unexpected(bad("settings", "must be an object"));
    }
    ExportSettings s;
    const std::string root;
    WABCUT_TRY(getString(j, "presetId", s.presetId, 128, root));
    WABCUT_TRY(getEnum(j, "container", s.container, containerFromName, root));
    if (const Json* v = member(j, "video")) {
        const std::string p = "video";
        WABCUT_TRY(requireObject(*v, p));
        VideoSettings& vs = s.video;
        WABCUT_TRY(getBool(*v, "enabled", vs.enabled, p));
        WABCUT_TRY(getEnum(*v, "codec", vs.codec, videoCodecFromName, p));
        WABCUT_TRY(getString(*v, "encoder", vs.encoder, 64, p));
        WABCUT_TRY(getEnum(*v, "rateControl", vs.rateControl, rateControlFromName, p));
        WABCUT_TRY(getIntAs(*v, "quality", vs.quality, 0, 51, p));
        WABCUT_TRY(getInt(*v, "bitrate", vs.bitrate, 0, 2'000'000'000, p));
        WABCUT_TRY(getInt(*v, "maxBitrate", vs.maxBitrate, 0, 4'000'000'000, p));
        WABCUT_TRY(getInt(*v, "targetBytes", vs.targetBytes, 0, 4'000'000'000, p));
        if (member(*v, "width") || member(*v, "height")) {
            int w = 0;
            int h = 0;
            WABCUT_TRY(getIntAs(*v, "width", w, 16, 8192, p));
            WABCUT_TRY(getIntAs(*v, "height", h, 16, 8192, p));
            vs.width = w;
            vs.height = h;
        }
        WABCUT_TRY(getEnum(*v, "scaleMode", vs.scaleMode, scaleModeFromName, p));
        if (const Json* rate = member(*v, "frameRate")) {
            std::int32_t num = 0;
            std::int32_t den = 0;
            WABCUT_TRY(getIntAs(*rate, "num", num, 1, 240'000, p + ".frameRate"));
            WABCUT_TRY(getIntAs(*rate, "den", den, 1, 1'001'000, p + ".frameRate"));
            if (num == 0 || den == 0) {
                return std::unexpected(bad(p + ".frameRate", "needs num and den"));
            }
            vs.frameRate = Rate{num, den}.normalized();
        }
        WABCUT_TRY(getEnum(*v, "pixelFormat", vs.pixelFormat, pixelFormatFromName, p));
        WABCUT_TRY(getEnum(*v, "proresProfile", vs.proresProfile, proresProfileFromName, p));
        WABCUT_TRY(getEnum(*v, "dnxhrProfile", vs.dnxhrProfile, dnxhrProfileFromName, p));
        WABCUT_TRY(getIntAs(*v, "keyframeInterval", vs.keyframeInterval.value, 1, Ticks::fromSeconds(3600).value, p));
        WABCUT_TRY(getEnum(*v, "dynamicRange", vs.dynamicRange, dynamicRangeFromName, p));
        WABCUT_TRY(getBool(*v, "exportAsSequence", vs.exportAsSequence, p));
    }
    if (const Json* a = member(j, "audio")) {
        const std::string p = "audio";
        WABCUT_TRY(requireObject(*a, p));
        WABCUT_TRY(getBool(*a, "enabled", s.audio.enabled, p));
        WABCUT_TRY(getEnum(*a, "codec", s.audio.codec, audioCodecFromKey, p));
        WABCUT_TRY(getIntAs(*a, "sampleRate", s.audio.sampleRate, 8000, 192000, p));
        WABCUT_TRY(getIntAs(*a, "channels", s.audio.channels, 0, 8, p));
        WABCUT_TRY(getInt(*a, "bitrate", s.audio.bitrate, 0, 2'000'000, p));
    }
    WABCUT_TRY(getEnum(j, "range", s.range, rangeModeFromName, root));
    if (const Json* r = member(j, "customRange")) {
        TimeRange range;
        WABCUT_TRY(getIntAs(*r, "start", range.start.value, 0, INT64_MAX / 4, "customRange"));
        WABCUT_TRY(getIntAs(*r, "duration", range.duration.value, 1, INT64_MAX / 4, "customRange"));
        s.customRange = range;
    }
    if (const Json* c = member(j, "captions")) {
        WABCUT_TRY(requireObject(*c, "captions"));
        WABCUT_TRY(getEnum(*c, "mode", s.captions.mode, captionModeFromName, "captions"));
        if (member(*c, "track")) {
            std::int64_t track = 0;
            WABCUT_TRY(getInt(*c, "track", track, 1, INT64_MAX, "captions"));
            s.captions.track = static_cast<std::uint64_t>(track);
        }
        WABCUT_TRY(getBool(*c, "karaoke", s.captions.karaoke, "captions"));
        WABCUT_TRY(getString(*c, "redactedWord", s.captions.redactedWord, 64, "captions"));
    }
    if (const Json* fx = member(j, "effects")) {
        WABCUT_TRY(readEffects(*fx, s.effects, "effects"));
    }
    if (const Json* m = member(j, "metadata")) {
        WABCUT_TRY(readMetadata(*m, s.metadata, "metadata"));
    }
    if (const Json* sub = member(j, "subtitles")) {
        WABCUT_TRY(readSubtitles(*sub, s.subtitles, "subtitles"));
    }
    if (const Json* r = member(j, "render")) {
        WABCUT_TRY(readRender(*r, s.render, "render"));
    }
    if (auto ok = validate(s); !ok) {
        return std::unexpected(ok.error());
    }
    return s;
}

std::expected<void, Error> applyOverrides(ExportSettings& s, const Json& overrides) {
    if (overrides.is_null()) {
        return {};
    }
    if (!overrides.is_object()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Export options must be an object"});
    }
    const auto has = [&](const char* key) { return member(overrides, key) != nullptr; };
    const auto value = [&](const char* key) -> const Json& { return *member(overrides, key); };

    // Container and codec first: they decide the defaults of the fields below.
    if (has("container")) {
        const auto c = containerFromName(textOf(value("container")));
        if (!c) {
            return std::unexpected(badOverride("container", value("container")));
        }
        s.container = *c;
        if (isAudioOnly(*c)) {
            s.video.enabled = false;
            if (*c == Container::Mp3) {
                s.audio.codec = AudioCodec::Mp3;
                s.audio.bitrate = std::clamp<std::int64_t>(s.audio.bitrate, 32'000, 320'000);
            } else if (*c == Container::M4a) {
                s.audio.codec = AudioCodec::Aac;
            } else if (s.audio.codec != AudioCodec::Pcm16 && s.audio.codec != AudioCodec::Pcm24) {
                s.audio.codec = AudioCodec::Pcm24; // WAV and AIFF carry PCM
            }
        } else if (*c == Container::Mp4 && s.audio.codec != AudioCodec::Aac) {
            s.audio.codec = AudioCodec::Aac; // MP4 carries AAC
        } else if (*c == Container::WebM) {
            s.audio.codec = AudioCodec::Opus; // WebM carries Opus
        } else if (*c == Container::Gif || *c == Container::Image) {
            s.audio.enabled = false; // a GIF or an image has no sound
        }
        if (*c == Container::Image && !s.video.enabled) {
            s.video.enabled = true;
        }
        if (*c == Container::Image && !(s.video.codec >= VideoCodec::Png && s.video.codec <= VideoCodec::Exr)) {
            s.video.codec = VideoCodec::Png; // images: PNG unless a still format is chosen
            s.video.pixelFormat = OutputPixelFormat::Rgb24;
        }
        if (s.audio.codec == AudioCodec::Mp3 && *c != Container::Mp3) {
            s.audio.codec = AudioCodec::Aac;
        }
    }
    if (has("videoCodec")) {
        const auto c = videoCodecFromName(textOf(value("videoCodec")));
        if (!c) {
            return std::unexpected(badOverride("videoCodec", value("videoCodec")));
        }
        if (*c != s.video.codec) {
            const bool alpha = hasAlpha(s.video.pixelFormat);
            s.video.codec = *c;
            s.video.encoder = "auto";
            s.video.dnxhrProfile = DnxhrProfile::Hq;
            s.video.proresProfile = alpha && *c == VideoCodec::ProRes ? ProResProfile::P4444 : ProResProfile::Hq;
            s.video.pixelFormat = defaultPixelFormat(*c, alpha, s.video); // codecs without alpha drop it
        }
    }
    if (has("video")) {
        s.video.enabled = boolOf(value("video"));
    }
    if (has("audio")) {
        s.audio.enabled = boolOf(value("audio"));
    }
    if (has("encoder")) {
        const std::string e = textOf(value("encoder"));
        if (e.size() > 64) {
            return std::unexpected(badOverride("encoder", value("encoder")));
        }
        s.video.encoder = e == "software" ? std::string(softwareEncoder(s.video.codec)) : e;
    }
    if (has("proresProfile")) {
        const auto p = proresProfileFromName(textOf(value("proresProfile")));
        if (!p) {
            return std::unexpected(badOverride("proresProfile", value("proresProfile")));
        }
        s.video.proresProfile = *p;
        if (s.video.codec == VideoCodec::ProRes) {
            const bool is4444 = *p == ProResProfile::P4444 || *p == ProResProfile::P4444Xq;
            s.video.pixelFormat = !is4444 ? OutputPixelFormat::Yuv422p10
                                  : hasAlpha(s.video.pixelFormat) ? OutputPixelFormat::Yuva444p10
                                                                  : OutputPixelFormat::Yuv444p10;
        }
    }
    if (has("dnxhrProfile")) {
        const auto p = dnxhrProfileFromName(textOf(value("dnxhrProfile")));
        if (!p) {
            return std::unexpected(badOverride("dnxhrProfile", value("dnxhrProfile")));
        }
        s.video.dnxhrProfile = *p;
        if (s.video.codec == VideoCodec::DnxHR) {
            s.video.pixelFormat = defaultPixelFormat(VideoCodec::DnxHR, false, s.video);
        }
    }
    if (has("pixelFormat")) {
        const auto f = pixelFormatFromName(textOf(value("pixelFormat")));
        if (!f) {
            return std::unexpected(badOverride("pixelFormat", value("pixelFormat")));
        }
        s.video.pixelFormat = *f;
    }
    if (has("alpha")) { // Premiere's Include Alpha Channel
        const bool alpha = boolOf(value("alpha"));
        const VideoCodec c = s.video.codec;
        if (alpha && c != VideoCodec::ProRes && c != VideoCodec::Png && c != VideoCodec::Tiff && c != VideoCodec::Targa &&
            c != VideoCodec::Animation && c != VideoCodec::Exr && c != VideoCodec::CineForm) {
            return std::unexpected(badOverride("alpha", value("alpha")));
        }
        if (alpha != hasAlpha(s.video.pixelFormat)) {
            using F = OutputPixelFormat;
            if (c == VideoCodec::ProRes) {
                if (alpha) {
                    s.video.proresProfile = s.video.proresProfile == ProResProfile::P4444Xq ? ProResProfile::P4444Xq : ProResProfile::P4444;
                }
                s.video.pixelFormat = alpha ? F::Yuva444p10 : F::Yuv444p10;
            } else if (s.video.pixelFormat == F::Rgb48 || s.video.pixelFormat == F::Rgba64) {
                s.video.pixelFormat = alpha ? F::Rgba64 : F::Rgb48;
            } else {
                s.video.pixelFormat = defaultPixelFormat(c, alpha, s.video);
            }
        }
    }
    if (has("exportAsSequence")) {
        s.video.exportAsSequence = boolOf(value("exportAsSequence"));
    }
    if (has("quality")) {
        const auto q = numberOf(value("quality"));
        if (!q) {
            return std::unexpected(badOverride("quality", value("quality")));
        }
        s.video.rateControl = RateControl::Quality;
        s.video.quality = static_cast<int>(std::clamp(*q, -1.0, 100.0));
    }
    if (has("bitrateMbps")) {
        const auto b = numberOf(value("bitrateMbps"));
        if (!b || *b < 0 || *b > 4000) {
            return std::unexpected(badOverride("bitrateMbps", value("bitrateMbps")));
        }
        s.video.rateControl = RateControl::Bitrate;
        s.video.bitrate = std::llround(*b * 1e6);
        s.video.maxBitrate = s.video.bitrate * 3 / 2;
    }
    if (has("maxBitrateMbps")) { // M5o: a tight cap for size targets (Discord); 0 = 1.5 x the bitrate
        const auto cap = numberOf(value("maxBitrateMbps"));
        if (!cap || *cap < 0 || *cap > 6000) {
            return std::unexpected(badOverride("maxBitrateMbps", value("maxBitrateMbps")));
        }
        s.video.maxBitrate = *cap == 0 ? s.video.bitrate * 3 / 2 : std::llround(*cap * 1e6);
    }
    if (has("frameRate")) { // M5o: the output frame rate; 0 = the sequence's
        const auto rate = parseFrameRate(value("frameRate"));
        if (!rate) {
            return std::unexpected(badOverride("frameRate", value("frameRate")));
        }
        s.video.frameRate = *rate;
    }
    if (has("width") || has("height")) {
        const auto w = has("width") ? numberOf(value("width")) : std::optional<double>(0.0);
        const auto h = has("height") ? numberOf(value("height")) : std::optional<double>(0.0);
        if (!w || !h || *w > 100'000 || *h > 100'000) {
            return std::unexpected(badOverride("width", has("width") ? value("width") : value("height")));
        }
        if (*w <= 0 || *h <= 0) { // 0 = the sequence size
            s.video.width.reset();
            s.video.height.reset();
        } else {
            s.video.width = static_cast<int>(*w);
            s.video.height = static_cast<int>(*h);
        }
    }
    if (has("scaleMode")) {
        const auto m = scaleModeFromName(textOf(value("scaleMode")));
        if (!m) {
            return std::unexpected(badOverride("scaleMode", value("scaleMode")));
        }
        s.video.scaleMode = *m;
    }
    if (has("range")) {
        const auto r = rangeModeFromName(textOf(value("range")));
        if (!r) {
            return std::unexpected(badOverride("range", value("range")));
        }
        s.range = *r;
    }
    if (has("customRange")) { // {start, end} in ticks (the front ends convert timecodes)
        const Json& cr = value("customRange");
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!cr.is_object() || !getInt(cr, "start", start, 0, INT64_MAX / 4, "customRange") ||
            !getInt(cr, "end", end, 0, INT64_MAX / 4, "customRange") || end <= start) {
            return std::unexpected(badOverride("customRange", cr));
        }
        s.customRange = TimeRange::fromStartEnd(Ticks{start}, Ticks{end});
        s.range = RangeMode::Custom;
    }
    if (has("captions")) { // I.4: off, burned in (the monitor's look), or a sidecar next to the file
        const std::string c = textOf(value("captions"));
        const auto mode = c == "off" ? std::optional<CaptionExport>(CaptionExport::None) : captionModeFromName(c);
        if (!mode) {
            return std::unexpected(badOverride("captions", value("captions")));
        }
        s.captions.mode = *mode;
    }
    if (has("subtitles")) { // 0.2: a soft subtitle track inside the file
        if (value("subtitles").is_object()) {
            if (auto ok = readSubtitles(value("subtitles"), s.subtitles, "subtitles"); !ok) {
                return ok;
            }
        } else {
            s.subtitles.enabled = boolOf(value("subtitles"));
        }
    }
    if (has("audioCodec")) {
        const std::string a = textOf(value("audioCodec"));
        const auto codec = audioCodecFromKey(a);
        if (!codec) {
            return std::unexpected(badOverride("audioCodec", value("audioCodec")));
        }
        // A.9: Dolby Digital (5.1); a WAV keeps its PCM.
        if (!(*codec == AudioCodec::Ac3 && s.container == Container::Wav)) {
            s.audio.codec = *codec;
        }
    }
    if (has("audioChannels")) { // A.9: 0 = as the master, 1, 2, 6
        const auto ch = numberOf(value("audioChannels"));
        if (!ch || (*ch != 0 && *ch != 1 && *ch != 2 && *ch != 6)) {
            return std::unexpected(badOverride("audioChannels", value("audioChannels")));
        }
        s.audio.channels = static_cast<int>(*ch);
    }
    if (has("sampleRate")) {
        const auto r = numberOf(value("sampleRate"));
        if (!r || *r < 0 || *r > 1'000'000) {
            return std::unexpected(badOverride("sampleRate", value("sampleRate")));
        }
        s.audio.sampleRate = static_cast<int>(*r);
    }
    if (has("audioBitrateKbps")) {
        const auto b = numberOf(value("audioBitrateKbps"));
        if (!b || *b < 0 || *b > 10'000) {
            return std::unexpected(badOverride("audioBitrateKbps", value("audioBitrateKbps")));
        }
        s.audio.bitrate = std::llround(*b * 1000.0);
    }
    if (has("dynamicRange")) { // 0.2 (I.9): Auto (the sequence's colour setup), SDR, HDR PQ or HDR HLG
        const auto d = dynamicRangeFromName(textOf(value("dynamicRange")));
        if (!d) {
            return std::unexpected(badOverride("dynamicRange", value("dynamicRange")));
        }
        s.video.dynamicRange = *d;
        // PQ and HLG are 10-bit: HEVC Main 10 and AV1 10-bit (ProRes is 10-bit already).
        if ((*d == DynamicRange::Pq || *d == DynamicRange::Hlg) && s.video.pixelFormat == OutputPixelFormat::Yuv420p &&
            (s.video.codec == VideoCodec::Hevc || s.video.codec == VideoCodec::Av1)) {
            s.video.pixelFormat = OutputPixelFormat::Yuv420p10;
        }
    }
    if (has("effects")) {
        ExportEffects fx;
        if (auto ok = readEffects(value("effects"), fx, "effects"); !ok) {
            return ok;
        }
        s.effects = std::move(fx);
    }
    if (has("metadata")) {
        ExportMetadata m;
        if (auto ok = readMetadata(value("metadata"), m, "metadata"); !ok) {
            return ok;
        }
        s.metadata = std::move(m);
    }
    if (has("render")) {
        ExportRenderOptions r;
        if (auto ok = readRender(value("render"), r, "render"); !ok) {
            return ok;
        }
        s.render = r;
    }
    return {};
}

// ---- Presets ------------------------------------------------------------------------------------------------------

std::string presetToJson(const Preset& preset) {
    const Json doc{{"format", kPresetFileFormat},
                   {"version", 1},
                   {"id", preset.id},
                   {"name", preset.name},
                   {"description", preset.description},
                   {"settings", settingsToJson(preset.settings)}};
    return doc.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
}

std::expected<Preset, Error> presetFromJson(std::string_view text) {
    if (text.size() > kMaxPresetFileBytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The preset file is too large"});
    }
    Json doc;
    try {
        doc = Json::parse(text.begin(), text.end());
    } catch (const std::exception&) {
        return std::unexpected(Error{ErrorCode::ParseError, "The preset file is not JSON"});
    }
    if (!doc.is_object() || !doc.contains("format") || !doc.at("format").is_string() ||
        doc.at("format").get<std::string>() != kPresetFileFormat) {
        return std::unexpected(Error{ErrorCode::ParseError, "Not a Wabcut export preset"});
    }
    std::int64_t version = 1;
    WABCUT_TRY(getInt(doc, "version", version, 1, 1000, ""));
    Preset p;
    WABCUT_TRY(getString(doc, "id", p.id, 128, ""));
    WABCUT_TRY(getString(doc, "name", p.name, 256, ""));
    WABCUT_TRY(getString(doc, "description", p.description, 1024, ""));
    if (p.name.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The preset has no name"});
    }
    const Json* settings = member(doc, "settings");
    if (!settings) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The preset has no settings"});
    }
    auto s = settingsFromJson(*settings);
    if (!s) {
        return std::unexpected(Error{s.error().code, "The preset's settings are invalid: " + s.error().message});
    }
    p.settings = std::move(*s);
    return p;
}

std::string userPresetId(std::string_view name) {
    std::string slug;
    bool dash = false;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0 && c < 0x80) {
            if (dash && !slug.empty()) {
                slug += '-';
            }
            slug += static_cast<char>(std::tolower(c));
            dash = false;
        } else {
            dash = true;
        }
        if (slug.size() >= 64) {
            break;
        }
    }
    if (slug.empty()) {
        // A name without ASCII letters or digits (e.g. Thai): a short stable hash of its bytes.
        std::uint64_t h = 1469598103934665603ULL;
        for (const char ch : name) {
            h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
        }
        slug = std::format("{:012x}", h & 0xFFFFFFFFFFFFULL);
    }
    return "user-" + slug;
}

PresetStore::PresetStore(std::filesystem::path folder) : m_folder(std::move(folder)) {}

std::filesystem::path PresetStore::fileFor(std::string_view id) const {
    return m_folder / pathFromUtf8(std::string(id) + ".json");
}

namespace {

std::expected<std::string, Error> readSmallFile(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::NotFound, "Cannot read " + pathToUtf8(file)});
    }
    if (size > kMaxPresetFileBytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The preset file is too large"});
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::NotFound, "Cannot read " + pathToUtf8(file)});
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool validUserId(std::string_view id) {
    if (!id.starts_with("user-") || id.size() > 5 + 64 || id.size() <= 5) {
        return false;
    }
    return std::all_of(id.begin() + 5, id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

} // namespace

std::vector<Preset> PresetStore::list() const {
    std::lock_guard lock(m_mutex);
    std::vector<Preset> out;
    m_problems.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(m_folder, ec)) {
        return out;
    }
    for (std::filesystem::directory_iterator it(m_folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& file = it->path();
        if (file.extension() != ".json" || !it->is_regular_file(ec)) {
            continue;
        }
        if (out.size() >= kMaxPresets) {
            m_problems.push_back("more than 1000 presets: the rest are left out");
            break;
        }
        auto text = readSmallFile(file);
        if (!text) {
            m_problems.push_back(std::format("{}: {}", pathToUtf8(file.filename()), text.error().message));
            continue;
        }
        auto preset = presetFromJson(*text);
        const std::string stem = pathToUtf8(file.stem());
        if (!preset) {
            m_problems.push_back(std::format("{}: {}", pathToUtf8(file.filename()), preset.error().message));
            continue;
        }
        if (preset->id != stem || !validUserId(preset->id)) {
            preset->id = validUserId(stem) ? stem : userPresetId(preset->name);
        }
        preset->settings.presetId = preset->id;
        out.push_back(std::move(*preset));
    }
    std::sort(out.begin(), out.end(), [](const Preset& a, const Preset& b) { return a.name < b.name; });
    return out;
}

std::vector<std::string> PresetStore::problems() const {
    std::lock_guard lock(m_mutex);
    return m_problems;
}

std::optional<Preset> PresetStore::find(std::string_view id) const {
    for (Preset& p : list()) {
        if (p.id == id) {
            return std::move(p);
        }
    }
    return std::nullopt;
}

std::optional<Preset> PresetStore::findAny(std::string_view id) const {
    if (auto builtIn = findPreset(id)) {
        return builtIn;
    }
    return find(id);
}

std::expected<Preset, Error> PresetStore::save(std::string_view name, std::string_view description, ExportSettings settings,
                                               bool overwrite) {
    std::string trimmed(name);
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
    if (trimmed.empty() || trimmed.size() > 256 || trimmed.find('\0') != std::string::npos) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "A preset needs a name of 1..256 bytes"});
    }
    if (description.size() > 1024) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "The preset description is at most 1024 bytes"});
    }
    if (auto ok = validate(settings); !ok) {
        return std::unexpected(ok.error());
    }
    Preset p;
    p.id = userPresetId(trimmed);
    p.name = trimmed;
    p.description = std::string(description);
    settings.presetId = p.id;
    p.settings = std::move(settings);
    std::lock_guard lock(m_mutex);
    std::error_code ec;
    const std::filesystem::path file = fileFor(p.id);
    if (!overwrite && std::filesystem::exists(file, ec)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     std::format("A preset named \"{}\" exists; replace it or choose another name", p.name)});
    }
    std::filesystem::create_directories(m_folder, ec);
    if (auto wrote = io::writeFileAtomically(file, presetToJson(p)); !wrote) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot save the preset: " + wrote.error().message});
    }
    return p;
}

std::expected<Preset, Error> PresetStore::importFile(const std::filesystem::path& file, bool overwrite) {
    auto text = readSmallFile(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto preset = presetFromJson(*text);
    if (!preset) {
        return std::unexpected(preset.error());
    }
    return save(preset->name, preset->description, std::move(preset->settings), overwrite);
}

std::expected<void, Error> PresetStore::exportFile(std::string_view id, const std::filesystem::path& file) const {
    const auto preset = findAny(id);
    if (!preset) {
        return std::unexpected(Error{ErrorCode::NotFound, std::format("No preset \"{}\"", id)});
    }
    if (auto wrote = io::writeFileAtomically(file, presetToJson(*preset)); !wrote) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot write the preset: " + wrote.error().message});
    }
    return {};
}

std::expected<void, Error> PresetStore::remove(std::string_view id) {
    if (!validUserId(id)) {
        return std::unexpected(Error{ErrorCode::NotFound,
                                     findPreset(id) ? "Built-in presets cannot be deleted" : std::format("No preset \"{}\"", id)});
    }
    std::lock_guard lock(m_mutex);
    std::error_code ec;
    const std::filesystem::path file = fileFor(id);
    if (!std::filesystem::exists(file, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, std::format("No preset \"{}\"", id)});
    }
    if (!std::filesystem::remove(file, ec) || ec) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot delete the preset: " + ec.message()});
    }
    return {};
}

} // namespace wabcut::exporter
