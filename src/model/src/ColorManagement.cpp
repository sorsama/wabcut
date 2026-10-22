// Colour management settings (ColorManagement.h): names and bounds.

#include <wabcut/model/ColorManagement.h>

#include <array>
#include <cmath>
#include <utility>

namespace wabcut {

namespace {

struct ModeName {
    ColorManagementMode mode;
    std::string_view id;
    std::string_view display;
    std::string_view description;
};
// Premiere Pro's Color Setup presets (Sequence Settings > Color), in its menu order.
constexpr std::array<ModeName, 6> kModes{{
    {ColorManagementMode::DisplayRec709, "rec709", "Direct Rec.709 (SDR)",
     "Rec.709 working space and output. SDR media passes through; HDR and log media are tone-mapped into SDR on input."},
    {ColorManagementMode::HdrHlg, "hdr-hlg", "Direct HLG (HDR)",
     "Rec.2020 HDR working space, HLG output. SDR media and graphics sit at the graphics white; the monitor shows a "
     "tone-mapped SDR preview."},
    {ColorManagementMode::HdrPq, "hdr-pq", "Direct PQ (HDR)",
     "Rec.2020 HDR working space, PQ output with HDR10 metadata. SDR media and graphics sit at the graphics white; the "
     "monitor shows a tone-mapped SDR preview."},
    {ColorManagementMode::Aces13, "aces", "Wide Gamut (Tone Mapped)",
     "ACES 1.3: an ACEScg working space that keeps every camera's gamut and range, the grade in ACEScct, the ACES output "
     "transforms tone-map the result."},
    {ColorManagementMode::WideGamut, "wide-gamut", "Wide Gamut (No Tone Mapping)",
     "The ACEScg working space with a straight conversion to the output: anything above the output's white is clipped."},
    {ColorManagementMode::Disabled, "off", "Disable Color Management",
     "No conversions: every picture is shown as its code values say, HDR and log media included (grade them by hand "
     "with LUTs)."},
}};

constexpr std::array<ColorManagementMode, 6> kModeMenu{
    ColorManagementMode::DisplayRec709, ColorManagementMode::HdrHlg,    ColorManagementMode::HdrPq,
    ColorManagementMode::Aces13,        ColorManagementMode::WideGamut, ColorManagementMode::Disabled,
};

struct SpaceName {
    InputColorSpace space;
    std::string_view id;
    std::string_view display;
};
constexpr std::array<SpaceName, 18> kSpaces{{
    {InputColorSpace::Rec709, "rec709", "Rec.709"},
    {InputColorSpace::Srgb, "srgb", "sRGB"},
    {InputColorSpace::LinearRec709, "linear-rec709", "Linear Rec.709"},
    {InputColorSpace::Rec2020, "rec2020", "Rec.2020 (SDR)"},
    {InputColorSpace::Rec2100Pq, "rec2100-pq", "Rec.2100 PQ"},
    {InputColorSpace::Rec2100Hlg, "rec2100-hlg", "Rec.2100 HLG"},
    {InputColorSpace::SLog3SGamut3, "s-log3", "Sony S-Log3 / S-Gamut3"},
    {InputColorSpace::SLog3SGamut3Cine, "s-log3-cine", "Sony S-Log3 / S-Gamut3.Cine"},
    {InputColorSpace::VLogVGamut, "v-log", "Panasonic V-Log / V-Gamut"},
    {InputColorSpace::LogC3, "logc3", "ARRI LogC3 (EI 800) / AWG3"},
    {InputColorSpace::LogC4, "logc4", "ARRI LogC4 / AWG4"},
    {InputColorSpace::CanonLog3, "canon-log3", "Canon Log 3 / Cinema Gamut"},
    {InputColorSpace::AppleLog, "apple-log", "Apple Log"},
    {InputColorSpace::AcesCg, "acescg", "ACEScg"},
    {InputColorSpace::Aces2065, "aces2065-1", "ACES2065-1"},
    {InputColorSpace::CanonLog2, "canon-log2", "Canon Log 2 / Cinema Gamut"},
    {InputColorSpace::RedLog3G10, "red-log3g10", "RED Log3G10 / REDWideGamutRGB"},
    {InputColorSpace::AcesCct, "acescct", "ACEScct"},
}};

constexpr std::array<InputColorSpace, 18> kMenuOrder{
    InputColorSpace::Rec709,       InputColorSpace::Srgb,         InputColorSpace::Rec2020,
    InputColorSpace::Rec2100Pq,    InputColorSpace::Rec2100Hlg,   InputColorSpace::SLog3SGamut3Cine,
    InputColorSpace::SLog3SGamut3, InputColorSpace::VLogVGamut,   InputColorSpace::LogC3,
    InputColorSpace::LogC4,        InputColorSpace::CanonLog3,    InputColorSpace::CanonLog2,
    InputColorSpace::RedLog3G10,   InputColorSpace::AppleLog,     InputColorSpace::LinearRec709,
    InputColorSpace::AcesCg,       InputColorSpace::AcesCct,      InputColorSpace::Aces2065,
};

const ModeName* modeEntry(ColorManagementMode mode) {
    for (const ModeName& m : kModes) {
        if (m.mode == mode) {
            return &m;
        }
    }
    return &kModes[0];
}

} // namespace

std::string_view toString(ColorManagementMode mode) { return modeEntry(mode)->id; }

std::optional<ColorManagementMode> colorManagementModeFromString(std::string_view text) {
    for (const ModeName& m : kModes) {
        if (m.id == text) {
            return m.mode;
        }
    }
    return std::nullopt;
}

std::string_view displayName(ColorManagementMode mode) { return modeEntry(mode)->display; }

std::string_view description(ColorManagementMode mode) { return modeEntry(mode)->description; }

std::span<const ColorManagementMode> allColorManagementModes() { return kModeMenu; }

std::string_view toString(InputColorSpace space) {
    for (const SpaceName& s : kSpaces) {
        if (s.space == space) {
            return s.id;
        }
    }
    return "rec709";
}

std::optional<InputColorSpace> inputColorSpaceFromString(std::string_view text) {
    for (const SpaceName& s : kSpaces) {
        if (s.id == text) {
            return s.space;
        }
    }
    return std::nullopt;
}

std::string_view displayName(InputColorSpace space) {
    for (const SpaceName& s : kSpaces) {
        if (s.space == space) {
            return s.display;
        }
    }
    return "Rec.709";
}

std::span<const InputColorSpace> allInputColorSpaces() {
    return kMenuOrder;
}

bool isCameraLog(InputColorSpace space) {
    switch (space) {
    case InputColorSpace::SLog3SGamut3:
    case InputColorSpace::SLog3SGamut3Cine:
    case InputColorSpace::VLogVGamut:
    case InputColorSpace::LogC3:
    case InputColorSpace::LogC4:
    case InputColorSpace::CanonLog3:
    case InputColorSpace::CanonLog2:
    case InputColorSpace::RedLog3G10:
    case InputColorSpace::AppleLog:
    case InputColorSpace::AcesCct: return true;
    default: return false;
    }
}

InputColorSpace autoInputColorSpace(const ColorInfo& color, bool isRgb) {
    switch (color.transfer) {
    case TransferFunction::PQ: return InputColorSpace::Rec2100Pq;
    case TransferFunction::HLG: return InputColorSpace::Rec2100Hlg;
    case TransferFunction::SRGB: return InputColorSpace::Srgb;
    case TransferFunction::Linear: return InputColorSpace::LinearRec709;
    case TransferFunction::Unknown:
        if (isRgb) {
            return InputColorSpace::Srgb;
        }
        break;
    default: break;
    }
    return color.primaries == ColorPrimaries::BT2020 ? InputColorSpace::Rec2020 : InputColorSpace::Rec709;
}

bool isRgbPixelFormat(std::string_view f) {
    for (const std::string_view prefix : {"rgb", "bgr", "argb", "abgr", "rgba", "bgra", "gbr", "0rgb", "0bgr", "x2rgb", "x2bgr", "pal8"}) {
        if (f.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

std::optional<InputColorSpace> detectedLogSpace(const VideoStreamInfo& stream) {
    if (stream.logColorSpace.empty()) {
        return std::nullopt;
    }
    const std::optional<InputColorSpace> space = inputColorSpaceFromString(stream.logColorSpace);
    return space && isCameraLog(*space) ? space : std::nullopt;
}

InputColorSpace autoInputColorSpace(const VideoStreamInfo& stream, bool detectLog) {
    if (detectLog) {
        if (const auto log = detectedLogSpace(stream)) {
            return *log;
        }
    }
    return autoInputColorSpace(stream.color, isRgbPixelFormat(stream.pixelFormat));
}

std::optional<std::string_view> colorManagementProblem(const ColorManagement& m) {
    if (!std::isfinite(m.peakNits) || m.peakNits < ColorManagement::kMinNits || m.peakNits > ColorManagement::kMaxNits) {
        return "the HDR peak must be 100 to 10000 nits";
    }
    if (!std::isfinite(m.referenceWhiteNits) || m.referenceWhiteNits < ColorManagement::kMinReferenceWhite ||
        m.referenceWhiteNits > ColorManagement::kMaxReferenceWhite) {
        return "the HDR reference white must be 80 to 400 nits";
    }
    if (m.referenceWhiteNits >= m.peakNits) {
        return "the HDR reference white must be below the peak";
    }
    return std::nullopt;
}

} // namespace wabcut
