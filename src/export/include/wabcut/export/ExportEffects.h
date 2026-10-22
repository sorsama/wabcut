#pragma once

// 0.2 (Premiere parity): what an export adds on top of the sequence - Premiere's Export Settings > Effects (Lumetri
// Look/LUT, Image Overlay, Name Overlay, Timecode Overlay, Video Limiter, Loudness Normalization), Metadata (chapters,
// the start timecode, XMP), a soft subtitle track, and the General / Video render toggles (Use Maximum Render Quality,
// Render at Maximum Depth, Use Previews, Smart Render). Plain data: ExportSettings holds them, ExportJob applies them
// (docs/export.md "Effects", "Metadata", "Smart rendering").
//
// The picture effects are applied to each output frame after the ScaleMode crop / pad (Premiere: "render overlays in
// the output frame size"), in this order: Lumetri Look/LUT, Video Limiter, then the overlays (image, name, timecode),
// so a look or the limiter never changes an overlay.

#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut::exporter {

/// Where an overlay sits in the output frame (Premiere's Position list).
enum class OverlayPosition : std::uint8_t {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

/// "topLeft", "topCenter", ..., "bottomRight".
std::string_view overlayPositionName(OverlayPosition position);
std::optional<OverlayPosition> overlayPositionFromName(std::string_view name);

/// Position, Offset, Size and Opacity of an overlay (Premiere's names).
struct OverlayPlacement {
    OverlayPosition position = OverlayPosition::BottomRight;
    /// Pixels of the output frame from the position, right and down positive (-4096..4096).
    int offsetX = 0;
    int offsetY = 0;
    /// Percent: an image at its own size (100), text at its default height (100 = 4.5 % of the frame height); 1..1000.
    double size = 100.0;
    double opacity = 100.0; ///< percent, 0..100

    bool operator==(const OverlayPlacement&) const = default;
};

/// Image Overlay (a watermark or a logo): a PNG / JPEG / ... image drawn over every frame.
struct ImageOverlay {
    bool applied = false;
    std::string image; ///< the image file (UTF-8 path; at most 4096 bytes)
    OverlayPlacement placement{OverlayPosition::BottomRight, -32, -32, 100.0, 100.0};

    bool operator==(const ImageOverlay&) const = default;
};

/// What a Name Overlay shows (Premiere: the source name or the output file name).
enum class NameOverlayFormat : std::uint8_t {
    SourceName,              ///< the sequence's name
    OutputName,              ///< the output file name without its extension
    OutputNameWithExtension, ///< the output file name
};
std::string_view nameOverlayFormatName(NameOverlayFormat format); ///< "sourceName", "outputName", "outputNameWithExtension"
std::optional<NameOverlayFormat> nameOverlayFormatFromName(std::string_view name);

/// Name Overlay: white text on a translucent black box (review copies).
struct NameOverlay {
    bool applied = false;
    std::string prefix; ///< at most 256 bytes each
    std::string suffix;
    NameOverlayFormat format = NameOverlayFormat::SourceName;
    OverlayPlacement placement{OverlayPosition::BottomCenter, 0, -48, 100.0, 100.0};

    bool operator==(const NameOverlay&) const = default;
};

/// Where a Timecode Overlay's time comes from (Premiere's Time Source).
enum class TimecodeSource : std::uint8_t {
    Sequence, ///< the sequence's timecode (its start timecode + the time), as the timeline shows it
    Generate, ///< counted from `startTimecode` at the first exported frame
};
/// How the time is shown (Premiere's Time Display).
enum class TimeDisplay : std::uint8_t {
    Timecode, ///< hh:mm:ss:ff (;ff drop-frame), in the output frame rate
    Frames,   ///< the frame number
};

/// Timecode Overlay: the time of each frame burned in (review copies, client notes).
struct TimecodeOverlay {
    bool applied = false;
    TimecodeSource source = TimecodeSource::Sequence;
    /// TimecodeSource::Generate: the time of the first exported frame (0..kMaxStartTimecode).
    Ticks startTimecode{};
    TimeDisplay display = TimeDisplay::Timecode;
    OverlayPlacement placement{OverlayPosition::TopCenter, 0, 32, 100.0, 100.0};

    bool operator==(const TimecodeOverlay&) const = default;
};

/// Lumetri Look/LUT on export: a look applied to the whole output (SDR exports).
struct LookExport {
    bool applied = false;
    /// "builtin:<name>" (color::builtinLuts(), looks and input LUTs) or a .cube file (UTF-8 path, at most 4096 bytes).
    std::string lut;

    bool operator==(const LookExport&) const = default;
};

/// Video Limiter: keeps R'G'B' (and so luma and chroma) at or under the Clip Level, a soft knee starting
/// `compression` percent under it (Premiere's Clip Level and Compression Before Clipping).
struct VideoLimiterExport {
    bool applied = false;
    double clipLevel = 100.0;  ///< IRE, 90..109 (100: the legal maximum; SDR exports never exceed 100)
    double compression = 3.0;  ///< percent, 0..20

    bool operator==(const VideoLimiterExport&) const = default;
};

/// The loudness standard of a Loudness Normalization (its target and true-peak ceiling).
enum class LoudnessStandard : std::uint8_t {
    EbuR128, ///< EBU R 128: -23 LUFS, -1 dBTP
    AtscA85, ///< ATSC A/85: -24 LKFS, -2 dBTP
    Custom,  ///< ITU-R BS.1770 with the target and ceiling below (streaming: -14 / -16 LUFS, -1 dBTP)
};
std::string_view loudnessStandardName(LoudnessStandard standard); ///< "ebuR128", "atscA85", "custom"
std::optional<LoudnessStandard> loudnessStandardFromName(std::string_view name);

/// Loudness Normalization: the exported mix measured (ITU-R BS.1770-4 integrated loudness, gated), then a gain moves it
/// to the target and a true-peak limiter keeps it under Max True Peak Level. Within Tolerance of the target, only the
/// limiter applies.
struct LoudnessNormalization {
    bool applied = false;
    LoudnessStandard standard = LoudnessStandard::EbuR128;
    double targetLufs = -23.0;     ///< Custom only (the standards fix theirs): -40..-5
    double toleranceLu = 1.0;      ///< 0..10
    double maxTruePeakDbtp = -1.0; ///< Custom only: -12..0
    bool writeLog = false;         ///< "<output>.loudness.txt": what was measured and done

    /// The target and ceiling the standard gives (Custom: the fields).
    double effectiveTarget() const;
    double effectiveTruePeak() const;
    bool operator==(const LoudnessNormalization&) const = default;
};

struct ExportEffects {
    LookExport look;
    VideoLimiterExport limiter;
    ImageOverlay image;
    NameOverlay name;
    TimecodeOverlay timecode;
    LoudnessNormalization loudness;

    /// Any effect on the picture (look, limiter, overlays).
    bool anyVideo() const { return look.applied || limiter.applied || image.applied || name.applied || timecode.applied; }
    bool operator==(const ExportEffects&) const = default;
};

/// What XMP an export carries (Premiere's Metadata Export Options).
enum class XmpMode : std::uint8_t {
    None,
    Embed,   ///< inside the file: MP4 / MOV as Adobe's XMP 'uuid' box; other formats fall back to a sidecar
    Sidecar, ///< "<output>.xmp" next to the file
};
std::string_view xmpModeName(XmpMode mode); ///< "none", "embed", "sidecar"
std::optional<XmpMode> xmpModeFromName(std::string_view name);

struct ExportMetadata {
    /// Chapter markers of the sequence in the exported range as chapters (MP4, MOV, MKV, M4A; times from the range start).
    bool chapters = true;
    /// The start timecode: a QuickTime timecode track ('tmcd') in MOV and MP4, the time reference of a Broadcast WAV.
    bool timecode = true;
    /// Premiere's Set Start Timecode: nullopt = the sequence's start timecode plus the range start.
    std::optional<Ticks> startTimecode;
    XmpMode xmp = XmpMode::None;
    /// XMP dc:title / dc:description (empty title = the sequence name); at most 1024 / 4096 bytes. MP4 / MOV / MKV also
    /// carry them as the file's title and description tags.
    std::string title;
    std::string description;
    /// The sequence markers of the range into the XMP (xmpDM:Tracks), as Premiere's "Include markers".
    bool xmpMarkers = true;

    bool operator==(const ExportMetadata&) const = default;
};

/// A soft subtitle track inside the file (the sequence's caption track, as CaptionExportSettings picks it: its track
/// and redacted word): 3GPP Timed Text (mov_text) in MP4 and MOV, SubRip in MKV. Players turn it on and off.
struct SubtitleTrackExport {
    bool enabled = false;
    std::string language; ///< ISO 639-2 code for the track ("" = the caption track's language, else "und")

    bool operator==(const SubtitleTrackExport&) const = default;
};

/// Premiere's render toggles.
struct ExportRenderOptions {
    /// Use Maximum Render Quality: an output smaller than the sequence is rendered at the sequence size and scaled down
    /// with a Lanczos filter, instead of composited at the output size.
    bool maximumRenderQuality = false;
    /// Render at Maximum Depth: 10-bit formats are rendered and read back at 10 bits per channel even for an 8-bit
    /// (display-referred) project.
    bool maximumDepth = false;
    /// Use Previews: frames inside rendered preview segments (C.15) that still show what the sequence shows are taken
    /// from the preview files, without rendering, when the export's format is the previews' (ProRes LT or DNxHR LB, the
    /// sequence's size and rate): their packets are copied (smart rendering).
    bool usePreviews = false;
    /// Smart Render: ProRes / DNxHR / DNxHD exports copy the packets of source clips that play untouched (one clip, full
    /// frame, no effects, its own rate and size, the same codec and profile) instead of decoding and encoding them.
    bool smartRender = true;

    bool operator==(const ExportRenderOptions&) const = default;
};

} // namespace wabcut::exporter
