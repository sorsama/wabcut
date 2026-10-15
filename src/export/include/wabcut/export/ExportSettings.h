#pragma once

// Export settings and presets (architecture rev 2, "export").
//
// Namespace note: `export` is a C++ keyword, so the namespace is wabcut::exporter (headers: wabcut/export/).

#include <wabcut/model/Error.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/Caption.h>

#include <cstdint>
#include <expected>
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
};

enum class VideoCodec : std::uint8_t {
    H264,
    Hevc,
    ProRes, ///< prores_ks
    DnxHR,  ///< dnxhd encoder with a DNxHR profile
    Av1,    ///< I.9: av1_nvenc / av1_qsv / av1_amf when present, else libsvtav1 (SVT-AV1); MP4 or MKV
    Gif,    ///< I.9 (gaming): FFmpeg's gif encoder, 256 colours per frame (palettegen / paletteuse); Container::Gif
    Vp9,    ///< I.9 (gaming): vp9_qsv when an Intel GPU has it, else libvpx-vp9; WebM or MKV
};

/// A.9: Ac3 is FFmpeg's native AC-3 encoder (up to 5.1).
enum class AudioCodec : std::uint8_t {
    Aac,
    Pcm16,
    Pcm24,
    Ac3,
    Opus, ///< I.9 (gaming): libopus, 48 kHz; WebM or MKV
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
};

enum class ProResProfile : std::uint8_t { Proxy, Lt, Standard, Hq };
enum class DnxhrProfile : std::uint8_t { Lb, Sq, Hq, Hqx };

enum class RangeMode : std::uint8_t {
    WholeSequence, ///< [0, sequence duration)
    InOut,         ///< the sequence In/Out marks (error if not both set)
    WorkArea,      ///< the work area (error if not set)
    Custom,        ///< ExportSettings::customRange
};

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

/// The sequence's captions in an export (I.4).
enum class CaptionExport : std::uint8_t {
    None,
    BurnIn, ///< drawn into the picture by the compositor, exactly as the monitor shows them
    Srt,    ///< a SubRip sidecar next to the output ("<output>.srt"), times relative to the exported range
    Vtt,    ///< a WebVTT sidecar ("<output>.vtt")
};

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

    bool operator==(const CaptionExportSettings&) const = default;
};

/// "none", "burnIn", "srt", "vtt".
std::string_view captionExportName(CaptionExport mode);

struct ExportSettings {
    std::string presetId; ///< the preset these settings came from (informational; empty = custom)
    Container container = Container::Mp4;
    VideoSettings video;
    AudioSettings audio;
    RangeMode range = RangeMode::WholeSequence;
    std::optional<TimeRange> customRange; ///< for RangeMode::Custom
    CaptionExportSettings captions;
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
/// "webm-vp9-720p".
std::vector<Preset> builtInPresets();
std::optional<Preset> findPreset(std::string_view id);

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
    Rate frameRate{}; ///< output frame rate
    /// Video frames: output frame k shows sequence time range.start + k frame periods; enough frames to cover
    /// the range (the last one may run past its end).
    std::int64_t frameCount = 0;
    /// Exported length: frameCount frame periods with video, the range length for audio-only exports.
    Ticks duration{};
    /// Audio sample frames: duration at the audio rate, rounded to nearest, so audio and video end together.
    std::int64_t audioSamples = 0;
    /// RateControl::TargetSize: the video bitrate (bits/s) that lands the file under VideoSettings::targetBytes.
    std::int64_t targetVideoBitrate = 0;
};

/// TargetSize: the share of the size kept back for the container and the encoders' overshoot, and the least video
/// bitrate an export may get (below it the range is too long for the size: resolve() says how long it may be).
inline constexpr double kTargetSizeMargin = 0.04;
inline constexpr std::int64_t kTargetSizeMinVideoBitrate = 150'000;

/// Resolves `settings` against `sequence` (its canvas, rates, marks and work area). Errors: InvalidArgument
/// (invalid settings, a range mode whose marks are not set, an empty range, a range too long for a target size).
std::expected<ResolvedExport, Error> resolve(const ExportSettings& settings, const Sequence& sequence);

std::string_view containerName(Container container); ///< "mp4", "mov", "mkv", "wav", "gif", "webm"
std::string_view fileExtension(Container container); ///< ".mp4", ".mov", ".mkv", ".wav", ".gif", ".webm"
std::string_view videoCodecName(VideoCodec codec);   ///< "h264", "hevc", "prores", "dnxhr", "av1", "gif", "vp9"
std::string_view audioCodecName(AudioCodec codec);
/// A.9: the channels an export of a sequence with this master format encodes (AudioSettings::channels resolved).
int exportAudioChannels(const AudioSettings& audio, ChannelFormat masterFormat);   ///< "aac", "pcm_s16le", "pcm_s24le"

/// How PCM exports quantize the float mix (FFmpeg's convention, round to nearest, clipped): `bits` 16 →
/// lrint(x · 2^15) in [-32768, 32767]; 24 → lrint(x · 2^23) in [-2^23, 2^23 - 1]. NaN is 0.
std::int32_t quantizePcmSample(float x, int bits);

} // namespace wabcut::exporter
