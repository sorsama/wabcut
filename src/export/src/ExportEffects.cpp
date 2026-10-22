#include <wabcut/export/ExportEffects.h>

#include "Extras.h"

#include <wabcut/model/Sequence.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace wabcut::exporter {

namespace {

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

constexpr std::array<std::string_view, 9> kPositionNames = {"topLeft",    "topCenter",    "topRight",
                                                             "centerLeft", "center",       "centerRight",
                                                             "bottomLeft", "bottomCenter", "bottomRight"};

bool finiteIn(double v, double lo, double hi) {
    return std::isfinite(v) && v >= lo && v <= hi;
}

std::expected<void, Error> checkPlacement(const OverlayPlacement& p, std::string_view what) {
    if (p.offsetX < -4096 || p.offsetX > 4096 || p.offsetY < -4096 || p.offsetY > 4096) {
        return std::unexpected(invalid(std::format("{}: the offset must be -4096..4096 pixels", what)));
    }
    if (!finiteIn(p.size, 1.0, 1000.0)) {
        return std::unexpected(invalid(std::format("{}: the size must be 1..1000 %", what)));
    }
    if (!finiteIn(p.opacity, 0.0, 100.0)) {
        return std::unexpected(invalid(std::format("{}: the opacity must be 0..100 %", what)));
    }
    return {};
}

bool hasNul(std::string_view s) {
    return s.find('\0') != std::string_view::npos;
}

} // namespace

std::string_view overlayPositionName(OverlayPosition position) {
    const auto i = static_cast<std::size_t>(position);
    return i < kPositionNames.size() ? kPositionNames[i] : kPositionNames[8];
}

std::optional<OverlayPosition> overlayPositionFromName(std::string_view name) {
    for (std::size_t i = 0; i < kPositionNames.size(); ++i) {
        if (kPositionNames[i] == name) {
            return static_cast<OverlayPosition>(i);
        }
    }
    return std::nullopt;
}

std::string_view nameOverlayFormatName(NameOverlayFormat format) {
    switch (format) {
    case NameOverlayFormat::SourceName:
        return "sourceName";
    case NameOverlayFormat::OutputName:
        return "outputName";
    case NameOverlayFormat::OutputNameWithExtension:
        return "outputNameWithExtension";
    }
    return "sourceName";
}

std::optional<NameOverlayFormat> nameOverlayFormatFromName(std::string_view name) {
    for (int f = 0; f <= static_cast<int>(NameOverlayFormat::OutputNameWithExtension); ++f) {
        if (nameOverlayFormatName(static_cast<NameOverlayFormat>(f)) == name) {
            return static_cast<NameOverlayFormat>(f);
        }
    }
    return std::nullopt;
}

std::string_view loudnessStandardName(LoudnessStandard standard) {
    switch (standard) {
    case LoudnessStandard::EbuR128:
        return "ebuR128";
    case LoudnessStandard::AtscA85:
        return "atscA85";
    case LoudnessStandard::Custom:
        return "custom";
    }
    return "ebuR128";
}

std::optional<LoudnessStandard> loudnessStandardFromName(std::string_view name) {
    for (int s = 0; s <= static_cast<int>(LoudnessStandard::Custom); ++s) {
        if (loudnessStandardName(static_cast<LoudnessStandard>(s)) == name) {
            return static_cast<LoudnessStandard>(s);
        }
    }
    return std::nullopt;
}

double LoudnessNormalization::effectiveTarget() const {
    switch (standard) {
    case LoudnessStandard::EbuR128:
        return -23.0;
    case LoudnessStandard::AtscA85:
        return -24.0;
    case LoudnessStandard::Custom:
        break;
    }
    return targetLufs;
}

double LoudnessNormalization::effectiveTruePeak() const {
    switch (standard) {
    case LoudnessStandard::EbuR128:
        return -1.0;
    case LoudnessStandard::AtscA85:
        return -2.0;
    case LoudnessStandard::Custom:
        break;
    }
    return maxTruePeakDbtp;
}

std::string_view xmpModeName(XmpMode mode) {
    switch (mode) {
    case XmpMode::None:
        return "none";
    case XmpMode::Embed:
        return "embed";
    case XmpMode::Sidecar:
        return "sidecar";
    }
    return "none";
}

std::optional<XmpMode> xmpModeFromName(std::string_view name) {
    for (int m = 0; m <= static_cast<int>(XmpMode::Sidecar); ++m) {
        if (xmpModeName(static_cast<XmpMode>(m)) == name) {
            return static_cast<XmpMode>(m);
        }
    }
    return std::nullopt;
}

std::expected<void, Error> validateExtras(const ExportSettings& s) {
    const ExportEffects& fx = s.effects;
    if (fx.anyVideo() && !s.video.enabled) {
        return std::unexpected(invalid("The picture effects (Lumetri Look/LUT, Video Limiter, overlays) need video"));
    }
    if (fx.look.applied) {
        if (fx.look.lut.empty() || fx.look.lut.size() > kMaxExportPathBytes || hasNul(fx.look.lut)) {
            return std::unexpected(invalid("Lumetri Look/LUT: choose a look or a .cube file"));
        }
    }
    if ((fx.look.applied || fx.limiter.applied) &&
        (s.video.dynamicRange == DynamicRange::Pq || s.video.dynamicRange == DynamicRange::Hlg)) {
        return std::unexpected(invalid("Lumetri Look/LUT and Video Limiter on export are for SDR exports"));
    }
    if (fx.limiter.applied &&
        (!finiteIn(fx.limiter.clipLevel, 90.0, 109.0) || !finiteIn(fx.limiter.compression, 0.0, 20.0))) {
        return std::unexpected(invalid("Video Limiter: the clip level is 90..109 IRE, the compression 0..20 %"));
    }
    if (fx.image.applied) {
        if (fx.image.image.empty() || fx.image.image.size() > kMaxExportPathBytes || hasNul(fx.image.image)) {
            return std::unexpected(invalid("Image Overlay: choose an image"));
        }
        if (auto ok = checkPlacement(fx.image.placement, "Image Overlay"); !ok) {
            return ok;
        }
    }
    if (fx.name.applied) {
        if (fx.name.prefix.size() > 256 || fx.name.suffix.size() > 256 || hasNul(fx.name.prefix) || hasNul(fx.name.suffix)) {
            return std::unexpected(invalid("Name Overlay: the prefix and suffix are at most 256 bytes"));
        }
        if (auto ok = checkPlacement(fx.name.placement, "Name Overlay"); !ok) {
            return ok;
        }
    }
    if (fx.timecode.applied) {
        if (fx.timecode.startTimecode.value < 0 || fx.timecode.startTimecode > kMaxStartTimecode) {
            return std::unexpected(invalid("Timecode Overlay: the starting timecode must be 00:00:00:00..23:59:59:xx"));
        }
        if (auto ok = checkPlacement(fx.timecode.placement, "Timecode Overlay"); !ok) {
            return ok;
        }
    }
    const LoudnessNormalization& ln = fx.loudness;
    if (ln.applied) {
        if (!s.audio.enabled) {
            return std::unexpected(invalid("Loudness Normalization needs audio"));
        }
        if (!finiteIn(ln.targetLufs, -40.0, -5.0) || !finiteIn(ln.toleranceLu, 0.0, 10.0) ||
            !finiteIn(ln.maxTruePeakDbtp, -12.0, 0.0)) {
            return std::unexpected(invalid(
                "Loudness Normalization: the target is -40..-5 LUFS, the tolerance 0..10 LU, the true peak -12..0 dBTP"));
        }
    }
    const ExportMetadata& m = s.metadata;
    if (m.title.size() > 1024 || m.description.size() > 4096 || hasNul(m.title) || hasNul(m.description)) {
        return std::unexpected(invalid("Metadata: the title is at most 1024 bytes, the description 4096"));
    }
    if (m.startTimecode && (m.startTimecode->value < 0 || *m.startTimecode > kMaxStartTimecode)) {
        return std::unexpected(invalid("Set Start Timecode must be 00:00:00:00..23:59:59:xx"));
    }
    if (s.subtitles.enabled) {
        if (!s.video.enabled ||
            (s.container != Container::Mp4 && s.container != Container::Mov && s.container != Container::Mkv)) {
            return std::unexpected(invalid("A subtitle track goes into an MP4, QuickTime or MKV video"));
        }
        const std::string& lang = s.subtitles.language;
        if (!lang.empty() && (lang.size() != 3 || !std::all_of(lang.begin(), lang.end(), [](char c) {
                                  return c >= 'a' && c <= 'z';
                              }))) {
            return std::unexpected(invalid("The subtitle language is an ISO 639-2 code (three letters, e.g. \"eng\")"));
        }
    }
    return {};
}

} // namespace wabcut::exporter
