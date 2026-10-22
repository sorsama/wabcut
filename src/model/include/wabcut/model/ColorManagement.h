#pragma once

// Colour management (I.9, back in 0.2; docs/color.md "Colour management"): the project's working space and how each
// clip's pictures are read into it. The modes are Premiere Pro's sequence "Color Setup" presets:
//
//   * ColorManagement (ProjectState::colorManagement):
//       - Direct Rec.709 (SDR), the default: a Rec.709 working space and output. SDR media passes through byte for
//         byte; HDR media (PQ / HLG) is tone-mapped into SDR on input (HDR game captures), and so are log clips (an
//         override, or log detected from the camera's metadata) through the ACES SDR output transform.
//       - Direct PQ (HDR) / Direct HLG (HDR): a linear Rec.2020 working space where 1.0 is the HDR reference white
//         (the graphics white), PQ or HLG masters; the monitor shows a tone-mapped SDR preview.
//       - Wide Gamut (Tone Mapped): ACES 1.3 - an ACEScg working space, the grade in ACEScct, the ACES output transforms.
//       - Wide Gamut (No Tone Mapping): the ACEScg working space without output tone mapping (Rec.709 out, clipped).
//       - Disable Color Management: every picture as its code values say, no conversion and no tone mapping at all.
//     Saved only when it is not the default.
//   * InputColorSpace (ClipContent::inputColorSpace, Premiere's "Override Media Color Space"): how a clip's decoded
//     code values are read. Auto (unset) follows the stream's tags (BT.709 / BT.2020, PQ / HLG, sRGB, linear) and, with
//     ColorManagement::autoDetectLog, the camera log the media's metadata names (VideoStreamInfo::logColorSpace); other
//     camera log formats are chosen here (S-Log3, V-Log, LogC3/4, Canon Log 2/3, RED Log3G10, Apple Log). Saved only
//     when set.
//
// Plain data with string names for the project file and the commands; render/color applies it.

#include <wabcut/model/MediaInfo.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace wabcut {

enum class ColorManagementMode : std::uint8_t {
    DisplayRec709, ///< default, Premiere's "Direct Rec.709 (SDR)": display-referred Rec.709 (SDR media byte for byte)
    Aces13,        ///< "Wide Gamut (Tone Mapped)": ACES 1.3, ACEScg working space, the grade in ACEScct, ACES output
    HdrPq,         ///< "Direct PQ (HDR)": HDR Rec.2100, PQ master: linear Rec.2020 working space, 1.0 = reference white
    HdrHlg,        ///< "Direct HLG (HDR)": HDR Rec.2100, HLG master (the same working space)
    WideGamut,     ///< "Wide Gamut (No Tone Mapping)": ACEScg working space, Rec.709 out without tone mapping (clipped)
    Disabled,      ///< "Disable Color Management": no input or output conversion, no tone mapping
};

enum class InputColorSpace : std::uint8_t {
    Rec709,           ///< BT.709 (also BT.601): the BT.709 curve, Rec.709 primaries
    Srgb,             ///< IEC 61966-2-1 sRGB (stills, screen recordings)
    LinearRec709,     ///< scene-linear, Rec.709 primaries
    Rec2020,          ///< BT.2020 SDR (the BT.709 curve), Rec.2020 primaries
    Rec2100Pq,        ///< BT.2100 PQ (SMPTE ST 2084), Rec.2020 primaries
    Rec2100Hlg,       ///< BT.2100 HLG, Rec.2020 primaries (read as a 1000-nit display)
    SLog3SGamut3,     ///< Sony S-Log3 / S-Gamut3
    SLog3SGamut3Cine, ///< Sony S-Log3 / S-Gamut3.Cine
    VLogVGamut,       ///< Panasonic V-Log / V-Gamut
    LogC3,            ///< ARRI LogC3 (EI 800) / ALEXA Wide Gamut 3
    LogC4,            ///< ARRI LogC4 / ARRI Wide Gamut 4
    CanonLog3,        ///< Canon Log 3 / Cinema Gamut
    AppleLog,         ///< Apple Log / Rec.2020
    AcesCg,           ///< ACEScg (linear AP1) renders
    Aces2065,         ///< ACES2065-1 (linear AP0) renders
    CanonLog2,        ///< Canon Log 2 / Cinema Gamut (0.2)
    RedLog3G10,       ///< RED Log3G10 / REDWideGamutRGB (0.2)
    AcesCct,          ///< ACEScct (log AP1) renders and grades (0.2)
};

/// The project's colour management. Default = Direct Rec.709 (SDR).
struct ColorManagement {
    static constexpr double kMinNits = 100.0;
    static constexpr double kMaxNits = 10000.0;
    static constexpr double kMinReferenceWhite = 80.0;
    static constexpr double kMaxReferenceWhite = 400.0;

    ColorManagementMode mode = ColorManagementMode::DisplayRec709;
    /// HDR: the peak luminance the programme is mastered for (nits): the mastering-display metadata of an HDR export
    /// and the knee of the tone-mapped SDR preview. 100..10000.
    double peakNits = 1000.0;
    /// HDR: where an SDR clip's, a title's or a caption's white lands (nits, ITU-R BT.2408: 203; Premiere's "graphics
    /// white"): 1.0 of the working space. 80..400.
    double referenceWhiteNits = 203.0;
    /// Premiere's "Color Manage Auto Detected Log and Raw Media": a clip without an override whose media names its camera
    /// log in its metadata (VideoStreamInfo::logColorSpace) is read as that log. Saved only when off.
    bool autoDetectLog = true;

    bool isDefault() const { return *this == ColorManagement{}; }
    bool isHdr() const { return mode == ColorManagementMode::HdrPq || mode == ColorManagementMode::HdrHlg; }
    /// A managed working space (Wide Gamut or HDR): every layer goes through an input plan, every output through an
    /// output plan. Direct Rec.709 and Disable Color Management keep the display-referred pipeline.
    bool managed() const { return mode != ColorManagementMode::DisplayRec709 && mode != ColorManagementMode::Disabled; }
    bool operator==(const ColorManagement&) const = default;
};

/// "rec709", "aces", "hdr-pq", "hdr-hlg", "wide-gamut", "off".
std::string_view toString(ColorManagementMode mode);
std::optional<ColorManagementMode> colorManagementModeFromString(std::string_view text);
std::string_view toString(InputColorSpace space);               ///< "rec709", "srgb", ..., "s-log3", "apple-log"
std::optional<InputColorSpace> inputColorSpaceFromString(std::string_view text);
/// A user-facing name (Premiere's preset names): "Direct Rec.709 (SDR)", "Wide Gamut (Tone Mapped)", "Direct PQ (HDR)";
/// "Rec.709", "Sony S-Log3 / S-Gamut3.Cine".
std::string_view displayName(ColorManagementMode mode);
std::string_view displayName(InputColorSpace space);
/// One line on what a mode does (the settings dialog, color.management.get).
std::string_view description(ColorManagementMode mode);
/// Every mode, in menu order (Premiere's Color Setup menu).
std::span<const ColorManagementMode> allColorManagementModes();
/// Every input colour space, in menu order.
std::span<const InputColorSpace> allInputColorSpaces();
/// A camera log (or ACEScct) encoding: read through an OpenColorIO transform into the working space.
bool isCameraLog(InputColorSpace space);

/// Auto: the input colour space a stream's colour tags imply. PQ / HLG transfer -> Rec.2100 PQ / HLG; sRGB and linear
/// transfers; BT.2020 primaries -> Rec.2020; otherwise Rec.709. `isRgb`: an RGB picture (a still, a graphic) whose
/// transfer is not tagged is sRGB. Camera log formats are never tagged (see detectedLogSpace).
InputColorSpace autoInputColorSpace(const ColorInfo& color, bool isRgb);
/// The same for a video stream (RGB when its pixel format is: rgb24, rgba, bgra, gbrp10le, pal8, ...), with the
/// camera log its metadata names when `detectLog` (ColorManagement::autoDetectLog).
InputColorSpace autoInputColorSpace(const VideoStreamInfo& stream, bool detectLog = true);
/// The camera log the stream's metadata names (VideoStreamInfo::logColorSpace), or nullopt.
std::optional<InputColorSpace> detectedLogSpace(const VideoStreamInfo& stream);
/// Whether an FFmpeg pixel format name is an RGB (or palette) format.
bool isRgbPixelFormat(std::string_view pixelFormat);

/// What is wrong with a colour-management setting (a value out of range), or nullopt.
std::optional<std::string_view> colorManagementProblem(const ColorManagement& management);

} // namespace wabcut
