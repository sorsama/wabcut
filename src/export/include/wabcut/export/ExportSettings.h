#pragma once

// Export settings and presets (architecture rev 2, "export").
//
// Namespace note: `export` is a C++ keyword, so the namespace is wabcut::exporter (headers: wabcut/export/).

#include <wabcut/export/ExportEffects.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/Caption.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::playback {
struct CaptionWordTimings;
}

namespace wabcut::exporter {

enum class Container : std::uint8_t {
    Mp4, ///< H.264/HEVC + AAC
    Mov, ///< H.264/HEVC/ProRes/DNxHR + AAC/PCM
    Mkv, ///< any codec here + AAC/PCM
    Wav, ///< audio only, PCM
    Gif, ///< I.9 (gaming): an animated GIF (memes): GIF video, no audio, loops forever
    WebM, ///< I.9 (gaming): VP9 or AV1 video, Opus audio (the web, chat apps, memes)
    // 0.2 (Premiere's formats):
    Mp3,   ///< audio only: MP3 (Media Foundation's encoder on Windows)
    M4a,   ///< audio only: AAC in an MPEG-4 audio file
    Aiff,  ///< audio only: big-endian PCM
    Image, ///< stills: one per frame with VideoSettings::exportAsSequence (numbered files), else the range's first frame
    // 0.6 (#73, broadcast and Avid delivery): MXF, PCM audio at 48 kHz, frame rates 23.976 / 24 / 25 / 29.97 / 30 / 50 /
    // 59.94 / 60.
    Mxf,       ///< MXF OP1a: one file - DNxHD / DNxHR, XDCAM HD422 or AVC-Intra 100 video, PCM tracks, a timecode track,
               ///< and the embedded captions as an SMPTE 436M ANC track (MxfSettings)
    MxfOpAtom, ///< MXF OP-Atom (Avid MediaFiles): one file per essence - "<name>.mxf" holds the DNxHD / DNxHR video,
               ///< "<name>_A1.mxf".. one mono PCM channel each; every file shares one material package UMID
};

enum class VideoCodec : std::uint8_t {
    H264,
    Hevc,
    ProRes, ///< prores_ks
    DnxHR,  ///< dnxhd encoder with a DNxHR profile
    Av1,    ///< I.9: av1_nvenc / av1_qsv / av1_amf when present, else libsvtav1 (SVT-AV1); MP4 or MKV
    Gif,    ///< I.9 (gaming): FFmpeg's gif encoder, 256 colours per frame (palettegen / paletteuse); Container::Gif
    Vp9,    ///< I.9 (gaming): vp9_qsv when an Intel GPU has it, else libvpx-vp9; WebM or MKV
    // 0.2: stills (Container::Image; PNG also in MOV, as QuickTime's PNG codec) and QuickTime's alpha codecs.
    Png,       ///< 8 / 16-bit RGB(A)
    Tiff,      ///< 8 / 16-bit RGB(A), LZW
    Dpx,       ///< 8 / 10 / 16-bit RGB
    Jpeg,      ///< 8-bit, quality = VideoSettings::quality on the 0..51 scale (lower is better)
    Targa,     ///< 8-bit RGB(A)
    Exr,       ///< OpenEXR, half float RGB(A), ZIP compression
    Animation, ///< QuickTime Animation (qtrle): 8-bit RGB(A), lossless; MOV
    CineForm,  ///< GoPro CineForm (cfhd): 4:2:2 10-bit, or 12-bit RGB(A); MOV
    // 0.6 (#73): broadcast codecs for MXF (also in MOV).
    Xdcam,    ///< XDCAM HD422: MPEG-2 4:2:2P@HL long GOP at 50 Mb/s CBR (mpeg2video), 8-bit 4:2:2; 1920x1080 at up to
              ///< 30 fps or 1280x720 at up to 60 fps
    AvcIntra, ///< AVC-Intra 100 (libx264, SMPTE RP 2027 class 100): 10-bit 4:2:2 intra; 1920x1080 at 25 / 29.97 or
              ///< 1280x720 at 50 / 59.94
};

/// A.9: Ac3 is FFmpeg's native AC-3 encoder (up to 5.1).
enum class AudioCodec : std::uint8_t {
    Aac,
    Pcm16,
    Pcm24,
    Ac3,
    Opus, ///< I.9 (gaming): libopus, 48 kHz; WebM or MKV
    Mp3,  ///< 0.2: MP3 (mp3_mf, Windows only), 32-48 kHz, mono or stereo, 32..320 kb/s; Container::Mp3
};

enum class RateControl : std::uint8_t {
    Quality,    ///< constant quality: CRF (x264/x265), CQ (NVENC), ICQ (QSV), CQP (AMF)
    Bitrate,    ///< VBR around `bitrate`, capped at `maxBitrate`
    TargetSize, ///< the whole file at most `targetBytes` (Discord's 10 / 25 / 50 MB): the video gets what the audio and
                ///< the container leave over the exported duration, its peaks capped at that rate; a file that still
                ///< ends over the size is encoded once more at the rate that fits (ExportResult::attempts)
};

/// How the sequence canvas is fitted into a frame size with another aspect ratio.
enum class ScaleMode : std::uint8_t {
    Fit,     ///< whole canvas visible, black bars (letter-/pillarbox)
    Fill,    ///< centre crop that fills the frame (export-side reframe, e.g. 9:16 from 16:9)
    Stretch, ///< distorts to the frame size
};

enum class OutputPixelFormat : std::uint8_t {
    Yuv420p,   ///< 8-bit 4:2:0 (delivery)
    Yuv420p10, ///< 10-bit 4:2:0 (HEVC Main 10)
    Yuv422p,   ///< 8-bit 4:2:2 (DNxHR SQ/HQ)
    Yuv422p10, ///< 10-bit 4:2:2 (ProRes 422, DNxHR HQX)
    Pal8,      ///< I.9: 8-bit palette, a palette of 256 colours made for each frame (GIF)
    // 0.2: 4:4:4 and RGB formats; the ones with "a" carry the alpha channel (Premiere's Include Alpha Channel).
    Yuv444p10,  ///< ProRes 4444, DNxHR 444
    Yuva444p10, ///< ProRes 4444 with alpha
    Rgb24,      ///< PNG, TIFF, DPX, Targa, JPEG, QuickTime Animation
    Rgba,       ///< PNG, TIFF, Targa, QuickTime Animation with alpha
    Rgb48,      ///< 16-bit: PNG, TIFF, DPX
    Rgba64,     ///< 16-bit with alpha: PNG, TIFF
    Gbrp10,     ///< 10-bit RGB: DPX
    Gbrp12,     ///< 12-bit RGB: CineForm
    Gbrap12,    ///< 12-bit RGB with alpha: CineForm
    Rgbf,       ///< half float: OpenEXR
    Rgbaf,      ///< half float with alpha: OpenEXR
};

/// The pixel format carries an alpha channel.
bool hasAlpha(OutputPixelFormat format);
/// "yuv420p", "yuv420p10", ..., "rgbaf".
std::string_view pixelFormatName(OutputPixelFormat format);

/// 0.2: P4444 / P4444Xq are ProRes 4444 and 4444 XQ (4:4:4 10-bit, alpha with Yuva444p10).
enum class ProResProfile : std::uint8_t { Proxy, Lt, Standard, Hq, P4444, P4444Xq };

/// What the picture is written as (I.9, docs/color.md "Colour management").
enum class DynamicRange : std::uint8_t {
    Auto, ///< the project's own: an HDR project's PQ or HLG when the format has 10 bits (HEVC, ProRes, AV1), else
          ///< SDR (its tone-mapped version); every other project SDR
    Sdr,  ///< Rec.709 (an ACES project through the ACES SDR output transform, an HDR one tone-mapped)
    Pq,   ///< BT.2100 PQ, Rec.2020, with HDR10 metadata (mastering display, MaxCLL / MaxFALL); HDR or ACES projects
    Hlg,  ///< BT.2100 HLG, Rec.2020; HDR or ACES projects
};

/// 0.7 (card "Dolby Vision 8.4 and HDR10+ dynamic metadata"): dynamic HDR metadata an export writes next to the picture.
/// HDR10+ (SMPTE ST 2094-40) is measured from the rendered frames, shot by shot, and written as a T.35 SEI message into
/// every access unit of a PQ HEVC video (MP4, MOV, MKV). Dolby Vision is licensed by Dolby: the writer sits behind
/// DolbyVisionRegistry (DynamicMetadata.h), and a build without a licensed one refuses it with the reason.
enum class DynamicMetadata : std::uint8_t {
    None,
    Hdr10Plus,     ///< HDR10+ on a PQ master
    DolbyVision81, ///< Dolby Vision profile 8.1: an HDR10 (PQ) base layer with an RPU per frame
    DolbyVision84, ///< Dolby Vision profile 8.4: an HLG base layer with an RPU per frame
};
/// "none", "hdr10plus", "dolbyVision81", "dolbyVision84" (the preset and override names).
std::string_view dynamicMetadataName(DynamicMetadata kind);
std::optional<DynamicMetadata> dynamicMetadataFromName(std::string_view name);
/// DNxHR LB / SQ / HQ (4:2:2 8-bit), HQX (4:2:2 10-bit), 444 (4:4:4 10-bit, 0.2), at any size; and (0.2) DNxHD, 1920x1080
/// only, at the bitrate Avid defines for the frame rate (dnxhdBitrate): LB 36/45/75/90, SQ 115/120/145/240/290, HQ
/// 175/185/220/365/440 Mb/s (8-bit), HQX 175x/185x/365x/440x (10-bit).
enum class DnxhrProfile : std::uint8_t { Lb, Sq, Hq, Hqx, Hr444, DnxhdLb, DnxhdSq, DnxhdHq, DnxhdHqx };

/// 0.2: the DNxHD bitrate (bits/s) of a DNxHD profile at 1920x1080 and `rate` (23.976 / 24 / 25 / 29.97 / 30 / 50 /
/// 59.94 / 60), or nullopt when Avid defines none (HQX at 29.97 / 30 in this FFmpeg; other sizes: use DNxHR).
std::optional<std::int64_t> dnxhdBitrate(DnxhrProfile profile, int width, int height, Rate rate);
bool isDnxhd(DnxhrProfile profile);

/// Premiere's Range: Entire Source, Source In/Out, Work Area, Custom.
enum class RangeMode : std::uint8_t {
    WholeSequence, ///< Entire Source: [0, the end of the last clip)
    InOut,         ///< Source In/Out: the sequence In/Out marks (error if not both set)
    WorkArea,      ///< Work Area (error if not set)
    Custom,        ///< Custom: ExportSettings::customRange
};
/// "whole", "inOut", "workArea", "custom" (the override and preset names).
std::string_view rangeModeName(RangeMode mode);
std::optional<RangeMode> rangeModeFromName(std::string_view name);

struct VideoSettings {
    bool enabled = true;
    VideoCodec codec = VideoCodec::H264;
    /// "auto": hardware encoders first (see hardwareEncoderOrder()), then the software encoder. Anything else
    /// is an FFmpeg encoder name used as is, without fallback (e.g. "libx264", "h264_nvenc").
    std::string encoder = "auto";
    RateControl rateControl = RateControl::Quality;
    /// Constant-quality level for H.264/HEVC, 0..51, lower is better (x264 CRF scale; mapped per encoder).
    int quality = 18;
    std::int64_t bitrate = 0;    ///< bits/s target for RateControl::Bitrate
    std::int64_t maxBitrate = 0; ///< bits/s cap for RateControl::Bitrate; 0 = 1.5 x bitrate
    std::int64_t targetBytes = 0; ///< RateControl::TargetSize: the most the whole file may weigh, in bytes
    /// Output frame size; nullopt = the sequence canvas.
    std::optional<int> width;
    std::optional<int> height;
    ScaleMode scaleMode = ScaleMode::Fit;
    /// Output frame rate; nullopt = the sequence frame rate.
    std::optional<Rate> frameRate;
    OutputPixelFormat pixelFormat = OutputPixelFormat::Yuv420p;
    ProResProfile proresProfile = ProResProfile::Hq;
    DnxhrProfile dnxhrProfile = DnxhrProfile::Hq;
    /// Keyframe interval of long-GOP codecs (H.264/HEVC/AV1).
    Ticks keyframeInterval = Ticks::fromSeconds(2);
    /// I.9: SDR, PQ or HLG (Auto: the project's own).
    DynamicRange dynamicRange = DynamicRange::Auto;
    /// 0.7: dynamic HDR metadata (HDR10+ on a PQ HEVC master; Dolby Vision where a licensed writer is registered).
    DynamicMetadata dynamicMetadata = DynamicMetadata::None;
    /// 0.6 (#72): the output colour space (Automatic: the sequence's colour setup's, ColorManagement::outputSpace) and
    /// SDR Conform (nullopt: the sequence's) of this export.
    OutputColorSpace outputSpace = OutputColorSpace::Automatic;
    std::optional<SdrConform> sdrConform;
    /// 0.2, Container::Image: Premiere's Export As Sequence - one numbered still per frame ("<name>000.png", ...);
    /// off, a single still of the range's first frame.
    bool exportAsSequence = true;
};

struct AudioSettings {
    bool enabled = true;
    AudioCodec codec = AudioCodec::Aac;
    int sampleRate = 48000;
    /// 1..8, or 0 = match the master (A.9): 6 (5.1) for a 5.1 master when the codec takes 5.1, else 2. A 5.1 master
    /// exported to fewer channels is downmixed (ITU-R BS.775).
    int channels = 2;
    std::int64_t bitrate = 320'000; ///< AAC and AC-3
};

/// The sequence's captions in an export (I.4; P.2: Premiere's sidecar formats and Embed In Output File).
enum class CaptionExport : std::uint8_t {
    None,
    BurnIn, ///< drawn into the picture by the compositor, exactly as the monitor shows them
    Srt,    ///< a SubRip sidecar next to the output ("<output>.srt"), times relative to the exported range
    Vtt,    ///< a WebVTT sidecar ("<output>.vtt")
    // P.2 sidecars (interchange/CaptionFiles.h), timecode-based ones from the sequence's start timecode:
    Scc,     ///< Scenarist Closed Caption (CEA-608, "<output>.scc")
    Mcc,     ///< MacCaption VANC (CEA-608 in CDPs, ".mcc")
    EbuStl,  ///< EBU N19 subtitles (".stl")
    Ttml,    ///< W3C Timed Text (".xml")
    Dfxp,    ///< DFXP (".dfxp")
    SmpteTt, ///< SMPTE-TT (".xml")
    Itt,     ///< iTunes Timed Text (".itt")
    /// Closed captions inside the output file (Premiere: Embed In Output File): CEA-608 and / or CEA-708
    /// (CaptionExportSettings::standard) in the H.264 / HEVC / MPEG-2 stream (ATSC A/53 caption data) and / or a
    /// caption track (MXF: SMPTE 436M ANC; QuickTime: c608) - CaptionExportSettings::carriage.
    Embed,
};

/// 0.6 (#73): which closed caption standards Embed writes. 608 is the analog line-21 format every decoder reads (CC1-CC4);
/// 708 the digital one (services 1-63, windows, pen attributes). "608 and 708" writes both, so older decoders keep the
/// 608 fallback.
enum class CaptionStandard : std::uint8_t { Cea608And708, Cea608, Cea708 };
/// 0.6 (#73): where Embed puts the captions. Auto: MXF -> its SMPTE 436M track; QuickTime -> the H.264 / HEVC stream
/// and (608 on CC1 / CC2) a c608 track, other codecs the c608 track alone; MP4 / MKV -> the video stream.
enum class CaptionCarriage : std::uint8_t {
    Auto,
    VideoStream,  ///< ATSC A/53 caption data in the picture stream (H.264 / HEVC SEI, MPEG-2 user data)
    CaptionTrack, ///< a track of their own (MXF: SMPTE 436M ANC, 608 and 708; QuickTime: c608, 608 CC1 / CC2)
    Both,
};
/// "608+708", "608", "708".
std::string_view captionStandardName(CaptionStandard standard);
std::optional<CaptionStandard> captionStandardFromName(std::string_view name);
/// "auto", "video", "track", "both".
std::string_view captionCarriageName(CaptionCarriage carriage);
std::optional<CaptionCarriage> captionCarriageFromName(std::string_view name);

/// The default replacement for a redacted word in exported captions (= playback::kRedactedWordToken).
inline constexpr std::string_view kRedactedWordToken = "[bleep]";

struct CaptionExportSettings {
    CaptionExport mode = CaptionExport::None;
    /// The caption track (CaptionTrackId::value); nullopt = the sequence's first enabled caption track.
    std::optional<std::uint64_t> track;
    /// Burn-in: how they look - unset = the caption track's own style (C.7, as the program monitor draws it), set =
    /// an explicit override - and karaoke highlighting.
    std::optional<render::CaptionStyle> style;
    bool karaoke = false;
    /// Karaoke word timings (caption track id and cue start -> word times): the same immutable map the program monitor
    /// highlights with, snapshotted into the request (Codex wave-3 P2-8). Null: estimated from each cue's duration,
    /// as the preview does without a transcript.
    std::shared_ptr<const playback::CaptionWordTimings> words;
    /// What each word the soundtrack bleeps or mutes becomes in burned-in captions and sidecars
    /// (playback/video/CaptionRedaction.h; Codex wave-4 P1-3).
    std::string redactedWord{kRedactedWordToken};
    /// SCC, MCC and Embed: how the 608 captions appear (interchange::Cea608Display: 0 pop-on, 1-3 roll-up 2/3/4 rows,
    /// 4 paint-on).
    std::uint8_t display608 = 0;
    /// 0.6 (#73), Embed: the standards, the 608 channel (1-4: CC1 / CC2 on field 1, CC3 / CC4 on field 2), the 708
    /// service (1-63) and where they go. 708 uses display608 too (pop-on: two windows that take turns; roll-up; paint-on).
    CaptionStandard standard = CaptionStandard::Cea608And708;
    std::uint8_t channel608 = 1;
    std::uint8_t service708 = 1;
    CaptionCarriage carriage = CaptionCarriage::Auto;

    bool operator==(const CaptionExportSettings&) const = default;
};

/// 0.6 (#73): how MXF OP1a lays out the audio channels. OP-Atom always writes one file per channel.
enum class MxfAudioLayout : std::uint8_t {
    MonoTracks,   ///< one mono PCM track per channel (AS-11 / DPP delivery, Avid)
    OneTrack,     ///< one multichannel PCM track
};
/// "mono", "multichannel".
std::string_view mxfAudioLayoutName(MxfAudioLayout layout);
std::optional<MxfAudioLayout> mxfAudioLayoutFromName(std::string_view name);

/// 0.6 (#73): MXF delivery options (Container::Mxf / ::MxfOpAtom).
struct MxfSettings {
    MxfAudioLayout audioLayout = MxfAudioLayout::MonoTracks;
    /// The tape / reel name written as the file's physical (tape) source package (Avid's Tape column); empty = none.
    /// At most 64 bytes.
    std::string reelName;

    bool operator==(const MxfSettings&) const = default;
};

/// "none", "burnIn", "srt", "vtt", "scc", "mcc", "stl", "ttml", "dfxp", "smpte-tt", "itt", "embed".
std::string_view captionExportName(CaptionExport mode);
/// The mode for a name above (case-sensitive), or nullopt.
std::optional<CaptionExport> captionExportFromName(std::string_view name);
/// Whether `mode` writes a caption file next to the output.
bool isCaptionSidecar(CaptionExport mode);

struct ExportSettings {
    std::string presetId; ///< the preset these settings came from (informational; empty = custom)
    Container container = Container::Mp4;
    VideoSettings video;
    AudioSettings audio;
    RangeMode range = RangeMode::WholeSequence;
    std::optional<TimeRange> customRange; ///< for RangeMode::Custom
    CaptionExportSettings captions;
    // 0.2 (ExportEffects.h): Premiere's Effects, Metadata, a soft subtitle track and the render toggles.
    ExportEffects effects;
    ExportMetadata metadata;
    SubtitleTrackExport subtitles;
    ExportRenderOptions render;
    MxfSettings mxf; ///< 0.6 (#73): MXF only
};

struct Preset {
    std::string id;          ///< stable, e.g. "youtube-1080p"
    std::string name;        ///< English UI name (translated by the app)
    std::string description; ///< English one-liner
    ExportSettings settings;
};

/// The built-in presets: "match-source" (Match Source, High), "youtube-1080p", "youtube-4k",
/// "vertical-1080x1920" (TikTok/Reels/Shorts 9:16, centre crop), "prores-422-hq" (ProRes 422 HQ master),
/// "audio-wav" (audio only); for gaming videos (I.9): "youtube-1080p60", "youtube-1440p60", "youtube-4k60"
/// (HEVC), "youtube-1440p60-av1", "youtube-4k60-av1", "shorts-1080x1920-60" (Shorts / TikTok / Reels at 60 fps),
/// "twitch-1080p60" (6 Mb/s), "discord-10mb", "discord-25mb", "discord-50mb" (target sizes), "gif-480p15" and
/// "webm-vp9-720p"; 0.2 (Premiere's formats): "prores-4444-alpha", "animation-alpha", "cineform-alpha", "dnxhr-hq",
/// "dnxhr-444", "dnxhd-1080p-hq", "audio-mp3", "audio-aac", "audio-aiff", "png-sequence", "tiff-sequence",
/// "dpx-sequence", "jpeg-sequence", "targa-sequence", "exr-sequence" and "png-still".
std::vector<Preset> builtInPresets();
std::optional<Preset> findPreset(std::string_view id);

/// "auto", "sdr", "pq", "hlg".
std::string_view dynamicRangeName(DynamicRange range);

/// Checks the settings on their own: codec/container/pixel-format combinations and value ranges.
/// Errors: InvalidArgument (message names the problem).
std::expected<void, Error> validate(const ExportSettings& settings);

/// What an export of `settings` from one sequence will actually write.
struct ResolvedExport {
    TimeRange range; ///< sequence time exported
    int width = 0;   ///< output frame size (even)
    int height = 0;
    int renderWidth = 0; ///< size the canvas is rendered at before the ScaleMode crop/pad (even)
    int renderHeight = 0;
    /// 145: the pixel aspect ratio the output file carries (reduced; 1/1 = square). The sequence's, when the output
    /// keeps the canvas's shape (the default size, or a pure scale of it); an output of another shape is made of square
    /// pixels (the canvas is resampled to its seen shape, ScaleMode fits that shape).
    Rate sampleAspect{1, 1};
    Rate frameRate{}; ///< output frame rate
    /// Video frames: output frame k shows sequence time range.start + k frame periods; enough frames to cover
    /// the range (the last one may run past its end).
    std::int64_t frameCount = 0;
    /// Exported length: frameCount frame periods with video, the range length for audio-only exports.
    Ticks duration{};
    /// Audio sample frames: duration at the audio rate, rounded to nearest, so audio and video end together.
    std::int64_t audioSamples = 0;
    /// I.9: what the picture is written as (never Auto): Sdr (BT.709 tags), Pq or Hlg (BT.2020 + ST 2084 / ARIB STD-B67).
    DynamicRange dynamicRange = DynamicRange::Sdr;
    /// 0.7: the dynamic metadata written (VideoSettings::dynamicMetadata, checked against the format: HDR10+ needs a PQ
    /// master on 10-bit HEVC in MP4, MOV or MKV).
    DynamicMetadata dynamicMetadata = DynamicMetadata::None;
    /// I.9: rendered and read back at 10 bits per channel (a colour-managed project into a 10-bit format). Exports of
    /// display-referred projects keep the 8-bit path they always had.
    bool tenBitRender = false;
    /// 0.6 (#72): the colour space written (never Automatic: Rec.709, Rec.2020 or P3-D65 for SDR; Rec.2100 PQ / HLG
    /// with those dynamic ranges), and SDR Conform when it applies (an SDR export of an HDR or Wide Gamut (No / Minimal
    /// Tone Mapping) setup with it on).
    OutputColorSpace outputSpace = OutputColorSpace::Rec709;
    std::optional<SdrConform> sdrConform;
    /// RateControl::TargetSize: the video bitrate (bits/s) that lands the file under VideoSettings::targetBytes.
    std::int64_t targetVideoBitrate = 0;
    /// 0.2: what the video source renders: the render size, or (Use Maximum Render Quality, an output smaller than the
    /// sequence) the sequence size, scaled down to the render size with a Lanczos filter (even).
    int sourceWidth = 0;
    int sourceHeight = 0;
    /// 0.2: the output carries an alpha channel (rendered with straight alpha, 8-bit).
    bool alpha = false;
};

/// TargetSize: the share of the size kept back for the container and the encoders' overshoot, and the least video
/// bitrate an export may get (below it the range is too long for the size: resolve() says how long it may be).
inline constexpr double kTargetSizeMargin = 0.04;
inline constexpr std::int64_t kTargetSizeMinVideoBitrate = 150'000;

/// Resolves `settings` against `sequence` (its canvas, rates, marks and work area) and the project's colour
/// management (DynamicRange::Auto, PQ / HLG only from an HDR or ACES project). Errors: InvalidArgument (invalid
/// settings, a range mode whose marks are not set, an empty range, PQ / HLG from a display-referred project).
std::expected<ResolvedExport, Error> resolve(const ExportSettings& settings, const Sequence& sequence,
                                             const ColorManagement& management = {});

/// "mp4", "mov", "mkv", "wav", "gif", "webm", "mp3", "m4a", "aiff", "image"; 0.6: "mxf" (OP1a), "mxf-opatom".
std::string_view containerName(Container container);
/// ".mp4", ".mov", ".mkv", ".wav", ".gif", ".webm", ".mp3", ".m4a", ".aif", ".png" (Image: see outputExtension), ".mxf".
std::string_view fileExtension(Container container);
/// 0.2: the extension of what `settings` write: fileExtension(container), or the still format's for Container::Image
/// (".png", ".tif", ".dpx", ".jpg", ".tga", ".exr").
std::string_view outputExtension(const ExportSettings& settings);
/// 148: `extension` (".mov", any case) is the file extension of a media or still format - one the export writes or a
/// neighbour (.mp4 .m4v .mov .mkv .webm .avi .mxf .wav .aif .aiff .mp3 .m4a .aac .flac .ogg .opus .gif .png .tif .tiff
/// .dpx .jpg .jpeg .tga .exr ...) - and not a piece of a name.
bool isMediaFileExtension(std::string_view extension);
/// 148: `extension` could be a file extension at all: a dot and 1 to 5 letters or digits that start with a letter.
/// ".03 Highlights" (from "Stream 2026.11.03 Highlights") and ".2" (from "cut v1.2") are not.
bool isPlausibleExtension(std::string_view extension);
/// 148: `path` with the format's `extension` (".mp4"): a current extension that is a media or still format's
/// (isMediaFileExtension) is swapped (.mov -> .mp4); anything else belongs to the name, so the format's extension is
/// appended ("Stream 2026.11.03 Highlights" -> "Stream 2026.11.03 Highlights.mp4", not "Stream 2026.11.mp4").
std::filesystem::path withOutputExtension(std::filesystem::path path, std::string_view extension);
/// "h264", "hevc", "prores", "dnxhr", "av1", "gif", "vp9", "png", "tiff", "dpx", "jpeg", "targa", "exr", "animation",
/// "cineform"; 0.6: "xdcam-hd422", "avc-intra-100".
std::string_view videoCodecName(VideoCodec codec);
std::optional<Container> containerFromName(std::string_view name);
std::optional<VideoCodec> videoCodecFromName(std::string_view name);
std::string_view audioCodecName(AudioCodec codec);
/// 0.2: "aac", "pcm16", "pcm24", "ac3", "opus", "mp3" (the override and preset names; audioCodecName is FFmpeg's).
std::string_view audioCodecKey(AudioCodec codec);
std::optional<AudioCodec> audioCodecFromKey(std::string_view key);
/// 0.2: the container holds audio only (WAV, MP3, M4A, AIFF).
bool isAudioOnly(Container container);
/// 0.6 (#73): MXF OP1a or OP-Atom.
bool isMxf(Container container);
/// 0.6 (#73): where an Embed export of `settings` puts its captions (CaptionExportSettings::carriage resolved for the
/// format), and what it writes there.
struct EmbeddedCaptionPlan {
    bool cea608 = false;
    bool cea708 = false;
    bool inVideo = false; ///< ATSC A/53 caption data in the picture stream
    bool track = false;   ///< a caption track: SMPTE 436M ANC (MXF) or c608 (QuickTime)
};
/// Errors: InvalidArgument naming why the format cannot carry what is asked (e.g. 708 in a QuickTime caption track,
/// captions in ProRes video, OP-Atom).
std::expected<EmbeddedCaptionPlan, Error> embeddedCaptionPlan(const ExportSettings& settings);
/// "CEA-608 CC1 + CEA-708 service 1 in the H.264 stream and an SMPTE 436M track" (the export details).
std::string describeEmbeddedCaptions(const ExportSettings& settings, const EmbeddedCaptionPlan& plan);
/// A.9: the channels an export of a sequence with this master format encodes (AudioSettings::channels resolved).
int exportAudioChannels(const AudioSettings& audio, ChannelFormat masterFormat);   ///< "aac", "pcm_s16le", "pcm_s24le"

/// How PCM exports quantize the float mix (FFmpeg's convention, round to nearest, clipped): `bits` 16 →
/// lrint(x · 2^15) in [-32768, 32767]; 24 → lrint(x · 2^23) in [-2^23, 2^23 - 1]. NaN is 0.
std::int32_t quantizePcmSample(float x, int bits);

} // namespace wabcut::exporter
