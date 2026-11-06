#pragma once

// RAW Source Settings (0.2, Premiere's Clip > Modify > Source Settings for camera RAW): the exposure and white balance a
// RAW clip's mosaic is developed with (media/RawDevelop.h) - ProRes RAW, CinemaDNG, and the RAW helpers that hand over
// the mosaic. They belong to the media item and travel in its locator path, as an image sequence's, a spanned clip's
// and a layered file's description do, so every decoder, frame cache, thumbnail, proxy and export develops the clip
// alike and the project file saves them with the item:
//
//     <file>?raw=exposure:<stops>[,white:<kelvin>:<tint>][,iso:<shot>:<develop>]   (a plain file)
//     <path with its own query>&raw=...                                           (a CinemaDNG image sequence)
//
// Exposure in stops (-5 .. +5, in hundredths); white balance As Shot (the camera's) unless `white` names the colour
// temperature (2000 .. 15000 K) and tint (-100 .. +100, + toward magenta) of the light to balance for; `iso` (0.7, card
// #108) the ISO the clip was shot at and the one to develop it for (50 .. 204800 each, different), which adds
// log2(develop / shot) stops. Default settings (0 stops, As Shot, no ISO change) leave the path bare. A build from before
// the `iso` part reads a path carrying it as a plain path (the suffix is not one it knows), so such a project must be
// opened again in this build. The rest of the path stands for the item wherever a file is checked
// (ImageSequence.h: mediaFileToCheck, relinkedMediaPath), and the other paths' parsers read past the suffix. Paths read
// from a project are untrusted: a suffix out of bounds is not one.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/ProjectState.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wabcut {

struct RawWhiteBalance {
    int temperature = 5600; ///< kelvin
    int tint = 0;           ///< + toward magenta, - toward green

    bool operator==(const RawWhiteBalance&) const = default;
};

/// 0.7 (card #108): ISO, in the photographer's terms. Wabcut cannot read the ISO a camera recorded (FFmpeg hands over the
/// ProRes RAW gain, not the ISO), so the item says which ISO it was shot at and which ISO to develop it for: the picture
/// gains log2(develop / shot) stops, +1 stop per doubling (a clip shot at 800 and developed for 1600 is a stop brighter,
/// as the camera's own ISO setting would have made it). Equal ISOs change nothing and are not stored.
struct RawIso {
    int shot = 800;    ///< the ISO the camera recorded at
    int develop = 800; ///< the ISO to develop for

    /// The exposure this ISO pair adds, stops.
    double stops() const;
    bool operator==(const RawIso&) const = default;
};

struct RawSourceSettings {
    double exposure = 0.0;                ///< stops: +1 doubles the light
    std::optional<RawWhiteBalance> white; ///< nullopt: As Shot
    std::optional<RawIso> iso;            ///< nullopt: the ISO the camera recorded, unchanged

    bool isDefault() const { return exposure == 0.0 && !white && !iso; }
    /// What the decoder develops with: the exposure plus the ISO's stops.
    double totalExposure() const { return exposure + (iso ? iso->stops() : 0.0); }
    bool operator==(const RawSourceSettings&) const = default;
};

inline constexpr double kMaxRawExposureStops = 5.0;
inline constexpr int kMinRawTemperature = 2000;
inline constexpr int kMaxRawTemperature = 15000;
inline constexpr int kMaxRawTint = 100;
inline constexpr int kMinRawIso = 50;
inline constexpr int kMaxRawIso = 204800;

/// Whether `settings` is within the bounds above.
bool validRawSourceSettings(const RawSourceSettings& settings);
/// The settings a media path carries (the default when it carries none).
RawSourceSettings rawSourceSettingsOf(std::string_view utf8);
/// The path without its RAW settings (itself when it carries none).
std::string_view withoutRawSourceSettings(std::string_view utf8);
/// `utf8` (any RAW settings it carries replaced) carrying `settings`, rounded to hundredths of a stop; the bare path
/// for the default settings. Empty when `settings` is out of bounds.
std::string withRawSourceSettings(std::string_view utf8, const RawSourceSettings& settings);
/// Whether an item's picture is a camera mosaic Wabcut develops: its video stream's pixel format is a Bayer one.
bool isRawMedia(const MediaInfo& info);

namespace edit {

/// Clip > Modify > Source Settings on RAW items: `media` are developed with `settings`. Each item's proxy is dropped
/// (it was made from the old development). One edit. Errors: NotFound, InvalidArgument (not a RAW item, settings out
/// of bounds).
EditOutcome setRawSourceSettings(const ProjectState& state, std::span<const MediaId> media,
                                 const RawSourceSettings& settings);

} // namespace edit

} // namespace wabcut
