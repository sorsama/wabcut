#include <wabcut/export/ExportSettings.h>

#include "Extras.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string>

namespace wabcut::exporter {

std::string_view captionExportName(CaptionExport mode) {
    switch (mode) {
    case CaptionExport::None:
        return "none";
    case CaptionExport::BurnIn:
        return "burnIn";
    case CaptionExport::Srt:
        return "srt";
    case CaptionExport::Vtt:
        return "vtt";
    case CaptionExport::Scc:
        return "scc";
    case CaptionExport::Mcc:
        return "mcc";
    case CaptionExport::EbuStl:
        return "stl";
    case CaptionExport::Ttml:
        return "ttml";
    case CaptionExport::Dfxp:
        return "dfxp";
    case CaptionExport::SmpteTt:
        return "smpte-tt";
    case CaptionExport::Itt:
        return "itt";
    case CaptionExport::Embed:
        return "embed";
    }
    return "none";
}

std::optional<CaptionExport> captionExportFromName(std::string_view name) {
    for (int m = 0; m <= static_cast<int>(CaptionExport::Embed); ++m) {
        if (captionExportName(static_cast<CaptionExport>(m)) == name) {
            return static_cast<CaptionExport>(m);
        }
    }
    return std::nullopt;
}

bool isCaptionSidecar(CaptionExport mode) {
    return mode != CaptionExport::None && mode != CaptionExport::BurnIn && mode != CaptionExport::Embed;
}

namespace {

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

bool is422(OutputPixelFormat f) {
    return f == OutputPixelFormat::Yuv422p || f == OutputPixelFormat::Yuv422p10;
}

bool oneOf(OutputPixelFormat f, std::initializer_list<OutputPixelFormat> formats) {
    return std::find(formats.begin(), formats.end(), f) != formats.end();
}

/// 0.2: the still and RGB codecs: which pixel formats they write, and the message when another one is asked for.
std::optional<std::string> stillFormatProblem(VideoCodec codec, OutputPixelFormat f) {
    using F = OutputPixelFormat;
    switch (codec) {
    case VideoCodec::Png:
    case VideoCodec::Tiff:
        if (!oneOf(f, {F::Rgb24, F::Rgba, F::Rgb48, F::Rgba64})) {
            return std::string(codec == VideoCodec::Png ? "PNG" : "TIFF") + " is 8 or 16-bit RGB, with or without alpha";
        }
        break;
    case VideoCodec::Dpx:
        if (!oneOf(f, {F::Rgb24, F::Gbrp10, F::Rgb48})) {
            return "DPX is 8, 10 or 16-bit RGB";
        }
        break;
    case VideoCodec::Jpeg:
        if (f != F::Rgb24) {
            return "JPEG is 8-bit RGB (no alpha)";
        }
        break;
    case VideoCodec::Targa:
    case VideoCodec::Animation:
        if (!oneOf(f, {F::Rgb24, F::Rgba})) {
            return std::string(codec == VideoCodec::Targa ? "Targa" : "QuickTime Animation") +
                   " is 8-bit RGB, with or without alpha";
        }
        break;
    case VideoCodec::Exr:
        if (!oneOf(f, {F::Rgbf, F::Rgbaf})) {
            return "OpenEXR is half float RGB, with or without alpha";
        }
        break;
    case VideoCodec::CineForm:
        if (!oneOf(f, {F::Yuv422p10, F::Gbrp12, F::Gbrap12})) {
            return "CineForm is 4:2:2 10-bit, or 12-bit RGB with or without alpha";
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

bool isStillCodec(VideoCodec c) {
    return c == VideoCodec::Png || c == VideoCodec::Tiff || c == VideoCodec::Dpx || c == VideoCodec::Jpeg ||
           c == VideoCodec::Targa || c == VideoCodec::Exr;
}

bool isNewPixelFormat(OutputPixelFormat f) {
    return static_cast<int>(f) > static_cast<int>(OutputPixelFormat::Pal8);
}

/// Nearest even integer, at least 2.
int roundEven(std::int64_t numerator, std::int64_t denominator) {
    const std::int64_t halfUnits =
        (2 * numerator + denominator) / (2 * denominator); // round(numerator / denominator)
    const std::int64_t even = halfUnits % 2 == 0 ? halfUnits : halfUnits + 1;
    return static_cast<int>(std::max<std::int64_t>(2, even));
}

ExportSettings h264Mp4() {
    ExportSettings s;
    s.container = Container::Mp4;
    s.video.codec = VideoCodec::H264;
    s.video.pixelFormat = OutputPixelFormat::Yuv420p;
    s.audio.codec = AudioCodec::Aac;
    return s;
}

} // namespace

std::vector<Preset> builtInPresets() {
    std::vector<Preset> presets;

    ExportSettings match = h264Mp4();
    match.presetId = "match-source";
    match.video.rateControl = RateControl::Quality;
    match.video.quality = 16;
    match.audio.bitrate = 320'000;
    presets.push_back({"match-source", "Match Source (High)",
                       "H.264 at the sequence size and frame rate, high constant quality", match});

    ExportSettings yt1080 = h264Mp4();
    yt1080.presetId = "youtube-1080p";
    yt1080.video.width = 1920;
    yt1080.video.height = 1080;
    yt1080.video.rateControl = RateControl::Bitrate;
    yt1080.video.bitrate = 16'000'000;
    yt1080.video.maxBitrate = 24'000'000;
    yt1080.audio.bitrate = 384'000;
    presets.push_back({"youtube-1080p", "YouTube 1080p", "H.264 1920×1080 at 16 Mb/s, AAC 384 kb/s", yt1080});

    ExportSettings yt4k = h264Mp4();
    yt4k.presetId = "youtube-4k";
    yt4k.video.width = 3840;
    yt4k.video.height = 2160;
    yt4k.video.rateControl = RateControl::Bitrate;
    yt4k.video.bitrate = 45'000'000;
    yt4k.video.maxBitrate = 68'000'000;
    yt4k.audio.bitrate = 384'000;
    presets.push_back({"youtube-4k", "YouTube 4K", "H.264 3840×2160 at 45 Mb/s, AAC 384 kb/s", yt4k});

    ExportSettings vertical = h264Mp4();
    vertical.presetId = "vertical-1080x1920";
    vertical.video.width = 1080;
    vertical.video.height = 1920;
    vertical.video.scaleMode = ScaleMode::Fill; // export-side reframe: centre crop
    vertical.video.rateControl = RateControl::Bitrate;
    vertical.video.bitrate = 12'000'000;
    vertical.video.maxBitrate = 18'000'000;
    presets.push_back({"vertical-1080x1920", "TikTok / Reels 9:16",
                       "H.264 1080×1920, centre crop of the canvas, 12 Mb/s", vertical});

    ExportSettings prores;
    prores.presetId = "prores-422-hq";
    prores.container = Container::Mov;
    prores.video.codec = VideoCodec::ProRes;
    prores.video.encoder = "auto";
    prores.video.proresProfile = ProResProfile::Hq;
    prores.video.pixelFormat = OutputPixelFormat::Yuv422p10;
    prores.audio.codec = AudioCodec::Pcm24;
    presets.push_back(
        {"prores-422-hq", "ProRes 422 HQ master", "ProRes 422 HQ 10-bit, 24-bit PCM, QuickTime", prores});

    ExportSettings wav;
    wav.presetId = "audio-wav";
    wav.container = Container::Wav;
    wav.video.enabled = false;
    wav.audio.codec = AudioCodec::Pcm24;
    presets.push_back({"audio-wav", "Audio only (WAV)", "24-bit PCM WAV at 48 kHz", wav});

    // Gaming videos (I.9): 60 fps YouTube uploads at YouTube's recommended SDR bitrates, Shorts, Twitch, Discord sizes.
    const auto sixty = [](ExportSettings s, const char* id, int w, int h, std::int64_t mbps) {
        s.presetId = id;
        s.video.width = w;
        s.video.height = h;
        s.video.frameRate = Rate{60, 1};
        s.video.rateControl = RateControl::Bitrate;
        s.video.bitrate = mbps * 1'000'000;
        s.video.maxBitrate = s.video.bitrate * 3 / 2;
        s.video.keyframeInterval = Ticks::fromSeconds(2);
        s.audio.bitrate = 384'000;
        return s;
    };
    ExportSettings hevc = h264Mp4();
    hevc.video.codec = VideoCodec::Hevc;
    ExportSettings av1 = h264Mp4();
    av1.video.codec = VideoCodec::Av1;
    presets.push_back({"youtube-1080p60", "YouTube 1080p60", "H.264 1920×1080 at 60 fps, 12 Mb/s, AAC 384 kb/s",
                       sixty(h264Mp4(), "youtube-1080p60", 1920, 1080, 12)});
    presets.push_back({"youtube-1440p60", "YouTube 1440p60", "H.264 2560×1440 at 60 fps, 24 Mb/s, AAC 384 kb/s",
                       sixty(h264Mp4(), "youtube-1440p60", 2560, 1440, 24)});
    presets.push_back({"youtube-4k60", "YouTube 4K60 (HEVC)", "HEVC 3840×2160 at 60 fps, 40 Mb/s, AAC 384 kb/s",
                       sixty(hevc, "youtube-4k60", 3840, 2160, 40)});
    presets.push_back({"youtube-1440p60-av1", "YouTube 1440p60 (AV1)",
                       "AV1 2560×1440 at 60 fps, 16 Mb/s (hardware AV1, else SVT-AV1), AAC 384 kb/s",
                       sixty(av1, "youtube-1440p60-av1", 2560, 1440, 16)});
    presets.push_back({"youtube-4k60-av1", "YouTube 4K60 (AV1)",
                       "AV1 3840×2160 at 60 fps, 30 Mb/s (hardware AV1, else SVT-AV1), AAC 384 kb/s",
                       sixty(av1, "youtube-4k60-av1", 3840, 2160, 30)});
    ExportSettings shorts = sixty(h264Mp4(), "shorts-1080x1920-60", 1080, 1920, 15);
    shorts.video.scaleMode = ScaleMode::Fill; // a 16:9 canvas is centre-cropped; a 9:16 sequence fits exactly
    shorts.audio.bitrate = 320'000;
    presets.push_back({"shorts-1080x1920-60", "Shorts / TikTok / Reels 60 fps",
                       "H.264 1080×1920 at 60 fps, 15 Mb/s, centre crop of a 16:9 canvas", shorts});
    ExportSettings twitch = sixty(h264Mp4(), "twitch-1080p60", 1920, 1080, 6);
    twitch.video.maxBitrate = twitch.video.bitrate; // Twitch's 6 Mb/s ceiling
    twitch.audio.bitrate = 160'000;
    presets.push_back({"twitch-1080p60", "Twitch 1080p60", "H.264 1920×1080 at 60 fps, 6 Mb/s, AAC 160 kb/s", twitch});
    // Discord: the whole file under the upload limit (sizes in decimal megabytes, the smaller reading of "MB").
    const auto discord = [](const char* id, int w, int h, std::int64_t megabytes, std::int64_t audio) {
        ExportSettings s = h264Mp4();
        s.presetId = id;
        s.video.width = w;
        s.video.height = h;
        s.video.rateControl = RateControl::TargetSize;
        s.video.targetBytes = megabytes * 1'000'000;
        s.audio.bitrate = audio;
        return s;
    };
    presets.push_back({"discord-10mb", "Discord 10 MB", "H.264 1280×720 sized to stay under 10 MB, AAC 96 kb/s",
                       discord("discord-10mb", 1280, 720, 10, 96'000)});
    presets.push_back({"discord-25mb", "Discord 25 MB", "H.264 1920×1080 sized to stay under 25 MB, AAC 128 kb/s",
                       discord("discord-25mb", 1920, 1080, 25, 128'000)});
    presets.push_back({"discord-50mb", "Discord 50 MB (Nitro Basic)",
                       "H.264 1920×1080 sized to stay under 50 MB, AAC 160 kb/s",
                       discord("discord-50mb", 1920, 1080, 50, 160'000)});
    // Memes: an animated GIF, 480 wide at 15 fps (a palette per frame).
    ExportSettings gif;
    gif.presetId = "gif-480p15";
    gif.container = Container::Gif;
    gif.video.codec = VideoCodec::Gif;
    gif.video.pixelFormat = OutputPixelFormat::Pal8;
    gif.video.width = 480;
    gif.video.height = 270;
    gif.video.frameRate = Rate{15, 1};
    gif.audio.enabled = false;
    presets.push_back({"gif-480p15", "GIF (memes)", "Animated GIF 480×270 at 15 fps, no sound, loops", gif});
    // The web and chat apps: WebM, VP9 at constant quality, Opus.
    ExportSettings webm;
    webm.presetId = "webm-vp9-720p";
    webm.container = Container::WebM;
    webm.video.codec = VideoCodec::Vp9;
    webm.video.width = 1280;
    webm.video.height = 720;
    webm.video.rateControl = RateControl::Quality;
    webm.video.quality = 26; // VP9 CRF 32
    webm.audio.codec = AudioCodec::Opus;
    webm.audio.bitrate = 128'000;
    presets.push_back({"webm-vp9-720p", "WebM VP9 720p (web, memes)", "VP9 1280×720 constant quality, Opus 128 kb/s", webm});

    // HDR (I.9, docs/color.md "Colour management"): from HDR Rec.2100 or ACES projects.
    ExportSettings hdrPq;
    hdrPq.presetId = "youtube-hdr-pq";
    hdrPq.container = Container::Mp4;
    hdrPq.video.codec = VideoCodec::Hevc;
    hdrPq.video.pixelFormat = OutputPixelFormat::Yuv420p10;
    hdrPq.video.dynamicRange = DynamicRange::Pq;
    hdrPq.video.quality = 18;
    hdrPq.audio.bitrate = 384'000;
    presets.push_back({"youtube-hdr-pq", "YouTube HDR (PQ)",
                       "HEVC Main 10, BT.2100 PQ with HDR10 metadata (hardware HEVC encoder), MP4", hdrPq});

    ExportSettings av1Pq = hdrPq;
    av1Pq.presetId = "youtube-hdr-pq-av1";
    av1Pq.video.codec = VideoCodec::Av1;
    presets.push_back({"youtube-hdr-pq-av1", "YouTube HDR (PQ, AV1)",
                       "AV1 10-bit, BT.2100 PQ with HDR10 metadata (hardware AV1, else SVT-AV1), MP4", av1Pq});

    ExportSettings hlg = hdrPq;
    hlg.presetId = "hdr-hlg";
    hlg.video.dynamicRange = DynamicRange::Hlg;
    presets.push_back({"hdr-hlg", "HDR HLG (HEVC 10-bit)", "HEVC Main 10, BT.2100 HLG (broadcast HDR), MP4", hlg});

    ExportSettings proresPq = prores;
    proresPq.presetId = "prores-hdr-pq";
    proresPq.video.dynamicRange = DynamicRange::Pq;
    presets.push_back({"prores-hdr-pq", "ProRes 422 HQ HDR master (PQ)",
                       "ProRes 422 HQ 10-bit, BT.2100 PQ, mastering display and MaxCLL / MaxFALL boxes, QuickTime",
                       proresPq});

    // 0.2 (Premiere's formats): alpha masters, DNxHR / DNxHD, audio only, image sequences and stills.
    const auto movVideo = [](const char* id, VideoCodec codec, OutputPixelFormat format) {
        ExportSettings s;
        s.presetId = id;
        s.container = Container::Mov;
        s.video.codec = codec;
        s.video.pixelFormat = format;
        s.audio.codec = AudioCodec::Pcm24;
        return s;
    };
    ExportSettings prores4444 = movVideo("prores-4444-alpha", VideoCodec::ProRes, OutputPixelFormat::Yuva444p10);
    prores4444.video.proresProfile = ProResProfile::P4444;
    presets.push_back({"prores-4444-alpha", "ProRes 4444 with alpha",
                       "ProRes 4444 10-bit 4:4:4 with the alpha channel (transparent titles, graphics), QuickTime", prores4444});
    presets.push_back({"animation-alpha", "QuickTime Animation with alpha",
                       "Lossless 8-bit RGB with the alpha channel (qtrle), QuickTime",
                       movVideo("animation-alpha", VideoCodec::Animation, OutputPixelFormat::Rgba)});
    presets.push_back({"cineform-alpha", "GoPro CineForm RGB 12-bit with alpha",
                       "CineForm 12-bit RGB with the alpha channel, QuickTime",
                       movVideo("cineform-alpha", VideoCodec::CineForm, OutputPixelFormat::Gbrap12)});
    ExportSettings dnxhrHq = movVideo("dnxhr-hq", VideoCodec::DnxHR, OutputPixelFormat::Yuv422p);
    dnxhrHq.video.dnxhrProfile = DnxhrProfile::Hq;
    presets.push_back({"dnxhr-hq", "DNxHR HQ", "DNxHR HQ 8-bit 4:2:2 at any size, 24-bit PCM, QuickTime", dnxhrHq});
    ExportSettings dnxhr444 = movVideo("dnxhr-444", VideoCodec::DnxHR, OutputPixelFormat::Yuv444p10);
    dnxhr444.video.dnxhrProfile = DnxhrProfile::Hr444;
    presets.push_back({"dnxhr-444", "DNxHR 444", "DNxHR 444 10-bit 4:4:4, 24-bit PCM, QuickTime", dnxhr444});
    ExportSettings dnxhd = movVideo("dnxhd-1080p-hq", VideoCodec::DnxHR, OutputPixelFormat::Yuv422p);
    dnxhd.video.dnxhrProfile = DnxhrProfile::DnxhdHq;
    dnxhd.video.width = 1920;
    dnxhd.video.height = 1080;
    presets.push_back({"dnxhd-1080p-hq", "DNxHD 1080p HQ",
                       "DNxHD 1920×1080 8-bit at Avid's HQ bitrate for the frame rate (175 / 185 / 220 / 365 / 440 Mb/s), "
                       "QuickTime",
                       dnxhd});

    const auto audioOnly = [](const char* id, Container container, AudioCodec codec, std::int64_t bitrate) {
        ExportSettings s;
        s.presetId = id;
        s.container = container;
        s.video.enabled = false;
        s.audio.codec = codec;
        s.audio.bitrate = bitrate;
        return s;
    };
    presets.push_back({"audio-mp3", "MP3 320 kb/s", "MP3 stereo, 320 kb/s, 48 kHz (Windows' MP3 encoder)",
                       audioOnly("audio-mp3", Container::Mp3, AudioCodec::Mp3, 320'000)});
    presets.push_back({"audio-aac", "AAC audio (M4A)", "AAC 256 kb/s, 48 kHz, MPEG-4 audio",
                       audioOnly("audio-aac", Container::M4a, AudioCodec::Aac, 256'000)});
    presets.push_back({"audio-aiff", "AIFF 24-bit", "24-bit PCM AIFF at 48 kHz",
                       audioOnly("audio-aiff", Container::Aiff, AudioCodec::Pcm24, 0)});

    const auto images = [](const char* id, VideoCodec codec, OutputPixelFormat format, bool sequence) {
        ExportSettings s;
        s.presetId = id;
        s.container = Container::Image;
        s.video.codec = codec;
        s.video.pixelFormat = format;
        s.video.exportAsSequence = sequence;
        s.video.quality = 4; // JPEG: high quality
        s.audio.enabled = false;
        return s;
    };
    presets.push_back({"png-sequence", "PNG sequence", "One 8-bit PNG with alpha per frame, numbered",
                       images("png-sequence", VideoCodec::Png, OutputPixelFormat::Rgba, true)});
    presets.push_back({"tiff-sequence", "TIFF sequence", "One 16-bit TIFF (LZW) per frame, numbered",
                       images("tiff-sequence", VideoCodec::Tiff, OutputPixelFormat::Rgb48, true)});
    presets.push_back({"dpx-sequence", "DPX sequence", "One 10-bit DPX per frame, numbered",
                       images("dpx-sequence", VideoCodec::Dpx, OutputPixelFormat::Gbrp10, true)});
    presets.push_back({"jpeg-sequence", "JPEG sequence", "One high-quality JPEG per frame, numbered",
                       images("jpeg-sequence", VideoCodec::Jpeg, OutputPixelFormat::Rgb24, true)});
    presets.push_back({"targa-sequence", "Targa sequence", "One 8-bit Targa with alpha per frame, numbered",
                       images("targa-sequence", VideoCodec::Targa, OutputPixelFormat::Rgba, true)});
    presets.push_back({"exr-sequence", "OpenEXR sequence", "One half-float OpenEXR (ZIP) per frame, numbered",
                       images("exr-sequence", VideoCodec::Exr, OutputPixelFormat::Rgbf, true)});
    presets.push_back({"png-still", "PNG still", "The first frame of the range as one PNG",
                       images("png-still", VideoCodec::Png, OutputPixelFormat::Rgb24, false)});
    return presets;
}

std::optional<Preset> findPreset(std::string_view id) {
    for (Preset& p : builtInPresets()) {
        if (p.id == id) {
            return std::move(p);
        }
    }
    return std::nullopt;
}

std::expected<void, Error> validate(const ExportSettings& s) {
    const VideoSettings& v = s.video;
    const AudioSettings& a = s.audio;
    if (!v.enabled && !a.enabled) {
        return std::unexpected(invalid("Nothing to export: video and audio are both off"));
    }
    // P.2: 608 captions are embedded in the H.264 / HEVC stream (A/53 caption SEI).
    if (s.captions.mode == CaptionExport::Embed &&
        (!v.enabled || (v.codec != VideoCodec::H264 && v.codec != VideoCodec::Hevc))) {
        return std::unexpected(invalid("Captions embed in H.264 or HEVC video: choose a sidecar file or burn them in"));
    }
    // Containers.
    if (s.container == Container::Gif) {
        if (!v.enabled || v.codec != VideoCodec::Gif) {
            return std::unexpected(invalid("A GIF holds GIF video"));
        }
        if (a.enabled) {
            return std::unexpected(invalid("A GIF has no sound: turn audio off"));
        }
    } else if (v.enabled && v.codec == VideoCodec::Gif) {
        return std::unexpected(invalid("GIF video goes into a .gif"));
    }
    if (s.container == Container::WebM) {
        if (v.enabled && v.codec != VideoCodec::Vp9 && v.codec != VideoCodec::Av1) {
            return std::unexpected(invalid("WebM holds VP9 or AV1 video"));
        }
        if (a.enabled && a.codec != AudioCodec::Opus) {
            return std::unexpected(invalid("WebM holds Opus audio"));
        }
    }
    if (v.enabled && v.codec == VideoCodec::Vp9 && s.container != Container::WebM && s.container != Container::Mkv) {
        return std::unexpected(invalid("VP9 goes into WebM or MKV"));
    }
    if (a.enabled && a.codec == AudioCodec::Opus && s.container != Container::WebM && s.container != Container::Mkv) {
        return std::unexpected(invalid("Opus goes into WebM or MKV"));
    }
    if (s.container == Container::Mov && v.enabled && v.codec == VideoCodec::Av1) {
        return std::unexpected(invalid("QuickTime does not hold AV1; use MP4 or MKV"));
    }
    // 0.2: audio-only formats, stills, QuickTime's alpha codecs.
    if (s.container == Container::Mp3 || s.container == Container::M4a || s.container == Container::Aiff) {
        if (v.enabled) {
            return std::unexpected(invalid("An audio file holds audio only: turn video off"));
        }
        if (s.container == Container::Mp3 && a.codec != AudioCodec::Mp3) {
            return std::unexpected(invalid("An MP3 file holds MP3 audio"));
        }
        if (s.container == Container::M4a && a.codec != AudioCodec::Aac) {
            return std::unexpected(invalid("An M4A file holds AAC audio"));
        }
        if (s.container == Container::Aiff && a.codec != AudioCodec::Pcm16 && a.codec != AudioCodec::Pcm24) {
            return std::unexpected(invalid("AIFF holds PCM audio"));
        }
    } else if (a.enabled && a.codec == AudioCodec::Mp3) {
        return std::unexpected(invalid("MP3 audio goes into an .mp3 file"));
    }
    if (s.container == Container::Image) {
        if (!v.enabled || !isStillCodec(v.codec)) {
            return std::unexpected(invalid("Images are PNG, TIFF, DPX, JPEG, Targa or OpenEXR"));
        }
        if (a.enabled) {
            return std::unexpected(invalid("Images have no sound: turn audio off"));
        }
    } else if (v.enabled && isStillCodec(v.codec) && !(v.codec == VideoCodec::Png && s.container == Container::Mov)) {
        return std::unexpected(invalid("Still formats are written as images (PNG also into QuickTime)"));
    }
    if (v.enabled && (v.codec == VideoCodec::Animation || v.codec == VideoCodec::CineForm) && s.container != Container::Mov) {
        return std::unexpected(invalid("QuickTime Animation and CineForm go into QuickTime (.mov)"));
    }
    if (s.container == Container::Wav) {
        if (v.enabled) {
            return std::unexpected(invalid("WAV holds audio only: turn video off"));
        }
        if (a.codec == AudioCodec::Aac || a.codec == AudioCodec::Ac3) {
            return std::unexpected(invalid("WAV holds PCM audio only"));
        }
    }
    if (s.container == Container::Mp4) {
        if (v.enabled && v.codec != VideoCodec::H264 && v.codec != VideoCodec::Hevc && v.codec != VideoCodec::Av1) {
            return std::unexpected(
                invalid("MP4 holds H.264, HEVC or AV1 video; use MOV or MKV for ProRes and DNxHR"));
        }
        if (a.enabled && a.codec != AudioCodec::Aac && a.codec != AudioCodec::Ac3) {
            return std::unexpected(invalid("MP4 holds AAC or AC-3 audio; use MOV, MKV or WAV for PCM"));
        }
    }
    // Video.
    if (v.enabled) {
        // 0.2: the 4:4:4 / RGB formats belong to their codecs (stillFormatProblem, ProRes 4444, DNxHR 444).
        if (isNewPixelFormat(v.pixelFormat) && !isStillCodec(v.codec) && v.codec != VideoCodec::Animation &&
            v.codec != VideoCodec::CineForm && v.codec != VideoCodec::ProRes && v.codec != VideoCodec::DnxHR) {
            return std::unexpected(invalid(std::format("{} is not written as {}", videoCodecName(v.codec),
                                                       pixelFormatName(v.pixelFormat))));
        }
        switch (v.codec) {
        case VideoCodec::H264:
            break;
        case VideoCodec::Hevc:
            if (v.pixelFormat == OutputPixelFormat::Yuv422p10) {
                return std::unexpected(invalid("HEVC export supports 4:2:0 8/10-bit and 4:2:2 8-bit"));
            }
            break;
        case VideoCodec::ProRes:
            if (v.proresProfile == ProResProfile::P4444 || v.proresProfile == ProResProfile::P4444Xq) {
                if (v.pixelFormat != OutputPixelFormat::Yuv444p10 && v.pixelFormat != OutputPixelFormat::Yuva444p10) {
                    return std::unexpected(invalid("ProRes 4444 is 4:4:4 10-bit (yuv444p10), with alpha yuva444p10"));
                }
            } else if (v.pixelFormat != OutputPixelFormat::Yuv422p10) {
                return std::unexpected(invalid("ProRes 422 is 4:2:2 10-bit (yuv422p10)"));
            }
            break;
        case VideoCodec::DnxHR: {
            using P = DnxhrProfile;
            const P p = v.dnxhrProfile;
            const OutputPixelFormat wanted = p == P::Hqx || p == P::DnxhdHqx ? OutputPixelFormat::Yuv422p10
                                             : p == P::Hr444                 ? OutputPixelFormat::Yuv444p10
                                                                             : OutputPixelFormat::Yuv422p;
            if (v.pixelFormat != wanted) {
                return std::unexpected(invalid("DNxHR LB/SQ/HQ are 4:2:2 8-bit (yuv422p), DNxHR HQX is 4:2:2 10-bit, "
                                               "DNxHR 444 4:4:4 10-bit; DNxHD LB/SQ/HQ 8-bit, HQX 10-bit"));
            }
            if (isDnxhd(p) && v.width && (*v.width != 1920 || *v.height != 1080)) {
                return std::unexpected(invalid("DNxHD is 1920×1080; use DNxHR for other sizes"));
            }
            break;
        }
        case VideoCodec::Png:
        case VideoCodec::Tiff:
        case VideoCodec::Dpx:
        case VideoCodec::Jpeg:
        case VideoCodec::Targa:
        case VideoCodec::Exr:
        case VideoCodec::Animation:
        case VideoCodec::CineForm:
            if (auto problem = stillFormatProblem(v.codec, v.pixelFormat)) {
                return std::unexpected(invalid(*problem));
            }
            if (v.codec == VideoCodec::Jpeg && (v.quality < 0 || v.quality > 51)) {
                return std::unexpected(invalid("Quality must be 0..51"));
            }
            break;
        case VideoCodec::Av1:
            if (is422(v.pixelFormat)) {
                return std::unexpected(invalid("AV1 export is 4:2:0 8-bit or 10-bit"));
            }
            break;
        case VideoCodec::Vp9:
            if (v.pixelFormat != OutputPixelFormat::Yuv420p) {
                return std::unexpected(invalid("VP9 export is 8-bit 4:2:0"));
            }
            break;
        case VideoCodec::Gif:
            if (v.pixelFormat != OutputPixelFormat::Pal8) {
                return std::unexpected(invalid("GIF is 8-bit palette (pal8)"));
            }
            if (v.frameRate && v.frameRate->toDouble() > 50.0) {
                return std::unexpected(invalid("GIF plays at most 50 fps (its frame times are hundredths of a second)"));
            }
            break;
        }
        if (v.rateControl == RateControl::TargetSize && v.codec != VideoCodec::H264 && v.codec != VideoCodec::Hevc &&
            v.codec != VideoCodec::Av1 && v.codec != VideoCodec::Vp9) {
            return std::unexpected(invalid("A target size needs H.264, HEVC, AV1 or VP9 video"));
        }
        if (v.pixelFormat == OutputPixelFormat::Pal8 && v.codec != VideoCodec::Gif) {
            return std::unexpected(invalid("An 8-bit palette (pal8) is for GIF only"));
        }
        if (v.dynamicRange == DynamicRange::Pq || v.dynamicRange == DynamicRange::Hlg) {
            const bool tenBit = v.pixelFormat == OutputPixelFormat::Yuv420p10 || v.pixelFormat == OutputPixelFormat::Yuv422p10 ||
                                v.pixelFormat == OutputPixelFormat::Yuv444p10;
            if (v.codec != VideoCodec::Hevc && v.codec != VideoCodec::ProRes && v.codec != VideoCodec::Av1) {
                return std::unexpected(invalid("PQ and HLG exports are HEVC, ProRes or AV1"));
            }
            if (!tenBit) {
                return std::unexpected(invalid("PQ and HLG exports are 10-bit (yuv420p10 or yuv422p10)"));
            }
        }
        if (v.width.has_value() != v.height.has_value()) {
            return std::unexpected(invalid("Set both the output width and height, or neither"));
        }
        if (v.width) {
            if (*v.width < 16 || *v.height < 16 || *v.width > 8192 || *v.height > 8192) {
                return std::unexpected(invalid("Output size must be 16..8192 pixels per side"));
            }
            if (*v.width % 2 != 0 || *v.height % 2 != 0) {
                return std::unexpected(invalid("Output width and height must be even (chroma subsampling)"));
            }
        }
        if (v.frameRate && (!v.frameRate->isValid() || v.frameRate->toDouble() > 240.0)) {
            return std::unexpected(invalid("Frame rate must be positive and at most 240 fps"));
        }
        if (v.codec == VideoCodec::H264 || v.codec == VideoCodec::Hevc || v.codec == VideoCodec::Av1 ||
            v.codec == VideoCodec::Vp9) {
            if (v.rateControl == RateControl::Quality && (v.quality < 0 || v.quality > 51)) {
                return std::unexpected(invalid("Quality must be 0..51"));
            }
            if (v.rateControl == RateControl::Bitrate && (v.bitrate < 100'000 || v.bitrate > 2'000'000'000 ||
                                                          (v.maxBitrate != 0 && v.maxBitrate < v.bitrate))) {
                return std::unexpected(
                    invalid("Bitrate must be 0.1..2000 Mb/s and the maximum at least the target"));
            }
            if (v.rateControl == RateControl::TargetSize && (v.targetBytes < 1'000'000 || v.targetBytes > 4'000'000'000)) {
                return std::unexpected(invalid("A target size must be 1 MB..4 GB"));
            }
            if (v.keyframeInterval.value <= 0) {
                return std::unexpected(invalid("Keyframe interval must be positive"));
            }
        }
    }
    // Audio.
    if (a.enabled) {
        constexpr int kRates[] = {32000, 44100, 48000, 88200, 96000};
        if (std::find(std::begin(kRates), std::end(kRates), a.sampleRate) == std::end(kRates)) {
            return std::unexpected(invalid("Audio sample rate must be 32, 44.1, 48, 88.2 or 96 kHz"));
        }
        if (a.channels < 0 || a.channels > 8) {
            return std::unexpected(invalid("Audio channels must be 1..8 (or 0: as the master)"));
        }
        if (a.codec == AudioCodec::Aac && (a.bitrate < 32'000 || a.bitrate > 640'000)) {
            return std::unexpected(invalid("AAC bitrate must be 32..640 kb/s"));
        }
        if (a.codec == AudioCodec::Opus) {
            if (a.sampleRate != 48000) {
                return std::unexpected(invalid("Opus is encoded at 48 kHz"));
            }
            if (a.bitrate < 32'000 || a.bitrate > 512'000) {
                return std::unexpected(invalid("Opus bitrate must be 32..512 kb/s"));
            }
        }
        if (a.codec == AudioCodec::Ac3) {
            if (a.channels > 6) {
                return std::unexpected(invalid("AC-3 carries at most 5.1 (6 channels)"));
            }
            if (a.sampleRate != 32000 && a.sampleRate != 44100 && a.sampleRate != 48000) {
                return std::unexpected(invalid("AC-3 takes 32, 44.1 or 48 kHz"));
            }
            if (a.bitrate < 32'000 || a.bitrate > 640'000) {
                return std::unexpected(invalid("AC-3 bitrate must be 32..640 kb/s"));
            }
        }
        if (a.codec == AudioCodec::Mp3) { // 0.2
            if (a.channels > 2) {
                return std::unexpected(invalid("MP3 is mono or stereo"));
            }
            if (a.sampleRate != 32000 && a.sampleRate != 44100 && a.sampleRate != 48000) {
                return std::unexpected(invalid("MP3 takes 32, 44.1 or 48 kHz"));
            }
            if (a.bitrate < 32'000 || a.bitrate > 320'000) {
                return std::unexpected(invalid("MP3 bitrate must be 32..320 kb/s"));
            }
        }
    }
    if (auto ok = validateExtras(s); !ok) { // 0.2: effects, metadata, subtitles, alpha
        return ok;
    }
    if (s.range == RangeMode::Custom &&
        (!s.customRange || s.customRange->isEmpty() || s.customRange->start.value < 0)) {
        return std::unexpected(invalid("A custom export range must be set, non-empty and not negative"));
    }
    return {};
}

std::expected<ResolvedExport, Error> resolve(const ExportSettings& s, const Sequence& sequence,
                                             const ColorManagement& management) {
    if (auto ok = validate(s); !ok) {
        return std::unexpected(ok.error());
    }
    ResolvedExport r;
    if (s.video.enabled) {
        // I.9: the dynamic range of the picture, and its precision.
        const bool managed = management.managed(); // Wide Gamut or HDR (not Direct Rec.709, not disabled)
        using F = OutputPixelFormat;
        const bool tenBit = oneOf(s.video.pixelFormat, {F::Yuv420p10, F::Yuv422p10, F::Yuv444p10});
        // 0.2: alpha exports render straight alpha at 8 bits (the 10-bit readback keeps 2 bits of alpha).
        r.alpha = hasAlpha(s.video.pixelFormat);
        const bool deep = !r.alpha && (tenBit || oneOf(s.video.pixelFormat, {F::Rgb48, F::Gbrp10, F::Gbrp12, F::Rgbf}));
        const bool hdrCodec = s.video.codec == VideoCodec::Hevc || s.video.codec == VideoCodec::ProRes || s.video.codec == VideoCodec::Av1;
        switch (s.video.dynamicRange) {
        case DynamicRange::Auto:
            r.dynamicRange = management.mode == ColorManagementMode::HdrPq && tenBit && hdrCodec    ? DynamicRange::Pq
                             : management.mode == ColorManagementMode::HdrHlg && tenBit && hdrCodec ? DynamicRange::Hlg
                                                                                                     : DynamicRange::Sdr;
            break;
        case DynamicRange::Sdr:
            r.dynamicRange = DynamicRange::Sdr;
            break;
        case DynamicRange::Pq:
        case DynamicRange::Hlg:
            if (!managed) {
                return std::unexpected(invalid("PQ and HLG exports need an HDR or Wide Gamut sequence colour setup (colour management)"));
            }
            r.dynamicRange = s.video.dynamicRange;
            break;
        }
        // 0.2: Render at Maximum Depth reads 10 bits back for any project into a format deeper than 8 bits.
        r.tenBitRender = deep && (managed || s.render.maximumDepth);
        if (r.alpha && r.dynamicRange != DynamicRange::Sdr) {
            return std::unexpected(invalid("Exports with an alpha channel are SDR"));
        }
        if ((s.effects.look.applied || s.effects.limiter.applied) && r.dynamicRange != DynamicRange::Sdr) {
            return std::unexpected(invalid("Lumetri Look/LUT and Video Limiter on export are for SDR exports"));
        }
    }
    switch (s.range) {
    case RangeMode::WholeSequence: {
        // Up to the last clip, as Premiere's Entire Sequence: ripple removals never shrink a sequence's duration
        // (SequenceEditor), so after a ripple or transcript delete the duration would add black and silence at the
        // end (RC.1). A sequence without clips keeps its duration.
        const Ticks content = sequence.contentEnd();
        r.range = TimeRange{Ticks{}, content.value > 0 ? content : sequence.duration};
        break;
    }
    case RangeMode::InOut:
        if (!sequence.markedRange()) {
            return std::unexpected(invalid("The sequence has no In and Out marks"));
        }
        r.range = *sequence.markedRange();
        break;
    case RangeMode::WorkArea:
        if (!sequence.workArea) {
            return std::unexpected(invalid("The sequence has no work area"));
        }
        r.range = *sequence.workArea;
        break;
    case RangeMode::Custom:
        r.range = *s.customRange;
        break;
    }
    if (r.range.isEmpty()) {
        return std::unexpected(invalid("The export range is empty"));
    }

    if (s.video.enabled) {
        const int canvasW = std::max(2, sequence.width - sequence.width % 2);
        const int canvasH = std::max(2, sequence.height - sequence.height % 2);
        r.width = s.video.width.value_or(canvasW);
        r.height = s.video.height.value_or(canvasH);
        const std::int64_t cw = sequence.width;
        const std::int64_t ch = sequence.height;
        // Output narrower than the canvas (relative to its height)?
        const bool narrower = std::int64_t{r.width} * ch < std::int64_t{r.height} * cw;
        switch (s.video.scaleMode) {
        case ScaleMode::Stretch:
            r.renderWidth = r.width;
            r.renderHeight = r.height;
            break;
        case ScaleMode::Fit: // scale by the smaller ratio: bars on the long side
            if (narrower) {
                r.renderWidth = r.width;
                r.renderHeight = std::min(r.height, roundEven(ch * r.width, cw));
            } else {
                r.renderHeight = r.height;
                r.renderWidth = std::min(r.width, roundEven(cw * r.height, ch));
            }
            break;
        case ScaleMode::Fill: // scale by the larger ratio: crop the long side
            if (narrower) {
                r.renderHeight = r.height;
                r.renderWidth = std::max(r.width, roundEven(cw * r.height, ch));
            } else {
                r.renderWidth = r.width;
                r.renderHeight = std::max(r.height, roundEven(ch * r.width, cw));
            }
            break;
        }
        // 0.2, Use Maximum Render Quality: an output smaller than the sequence is rendered at the sequence size (same
        // aspect as the render size) and scaled down with Lanczos.
        r.sourceWidth = r.renderWidth;
        r.sourceHeight = r.renderHeight;
        if (s.render.maximumRenderQuality && r.renderWidth < canvasW && r.renderHeight < canvasH) {
            r.sourceWidth = canvasW;
            r.sourceHeight = std::max(2, roundEven(std::int64_t{canvasW} * r.renderHeight, r.renderWidth));
        }
        r.frameRate = s.video.frameRate.value_or(sequence.frameRate);
        auto frames = ticksToFrames(r.range.duration, r.frameRate, Rounding::Ceil);
        if (frames && s.container == Container::Image && !s.video.exportAsSequence) {
            frames = std::int64_t{1}; // 0.2: a single still of the range's first frame
        }
        const auto duration = frames ? framesToTicks(*frames, r.frameRate, Rounding::Nearest) : std::nullopt;
        if (!frames || !duration) {
            return std::unexpected(invalid("The export range is too long for its frame rate"));
        }
        r.frameCount = *frames;
        r.duration = *duration;
        if (s.video.codec == VideoCodec::DnxHR && isDnxhd(s.video.dnxhrProfile) &&
            !dnxhdBitrate(s.video.dnxhrProfile, r.width, r.height, r.frameRate)) {
            return std::unexpected(invalid(std::format(
                "This DNxHD profile is not defined at {}×{} and {:.3g} fps (DNxHD is 1920×1080 at 23.976 to 60 fps; HQX "
                "not at 29.97 / 30): use DNxHR",
                r.width, r.height, r.frameRate.toDouble())));
        }
    } else {
        r.duration = r.range.duration;
    }
    if (s.audio.enabled) {
        const auto samples = rescale(r.duration, TimeBase{1, s.audio.sampleRate}, Rounding::Nearest);
        if (!samples) {
            return std::unexpected(invalid("The export range is too long for its sample rate"));
        }
        r.audioSamples = *samples;
    }
    if (s.video.enabled && s.video.rateControl == RateControl::TargetSize) {
        // The size less the margin, less the audio (PCM at its exact rate), spread over the duration.
        const double seconds = std::max(r.duration.toSeconds(), 0.001);
        const int channels = std::max(1, s.audio.channels);
        const double audioBits = !s.audio.enabled ? 0.0
                                 : s.audio.codec == AudioCodec::Pcm16 ? 16.0 * s.audio.sampleRate * channels * seconds
                                 : s.audio.codec == AudioCodec::Pcm24 ? 24.0 * s.audio.sampleRate * channels * seconds
                                                                      : static_cast<double>(s.audio.bitrate) * seconds;
        const double budget = static_cast<double>(s.video.targetBytes) * 8.0 * (1.0 - kTargetSizeMargin) - audioBits;
        r.targetVideoBitrate = static_cast<std::int64_t>(budget / seconds);
        if (r.targetVideoBitrate < kTargetSizeMinVideoBitrate) {
            const double perSecond = static_cast<double>(kTargetSizeMinVideoBitrate) + audioBits / seconds;
            const double longest = static_cast<double>(s.video.targetBytes) * 8.0 * (1.0 - kTargetSizeMargin) / perSecond;
            return std::unexpected(invalid(std::format(
                "The export is too long for {:.0f} MB: {:.0f} s fits at most (it is {:.0f} s); export a shorter range or a "
                "bigger size",
                static_cast<double>(s.video.targetBytes) / 1e6, std::floor(longest), std::ceil(seconds))));
        }
    }
    return r;
}

std::string_view containerName(Container c) {
    switch (c) {
    case Container::Mp4:
        return "mp4";
    case Container::Mov:
        return "mov";
    case Container::Mkv:
        return "mkv";
    case Container::Wav:
        return "wav";
    case Container::Gif:
        return "gif";
    case Container::WebM:
        return "webm";
    case Container::Mp3:
        return "mp3";
    case Container::M4a:
        return "m4a";
    case Container::Aiff:
        return "aiff";
    case Container::Image:
        return "image";
    }
    return "mp4";
}

std::string_view fileExtension(Container c) {
    switch (c) {
    case Container::Mp4:
        return ".mp4";
    case Container::Mov:
        return ".mov";
    case Container::Mkv:
        return ".mkv";
    case Container::Wav:
        return ".wav";
    case Container::Gif:
        return ".gif";
    case Container::WebM:
        return ".webm";
    case Container::Mp3:
        return ".mp3";
    case Container::M4a:
        return ".m4a";
    case Container::Aiff:
        return ".aif";
    case Container::Image:
        return ".png";
    }
    return ".mp4";
}

std::string_view outputExtension(const ExportSettings& s) {
    if (s.container != Container::Image) {
        return fileExtension(s.container);
    }
    switch (s.video.codec) {
    case VideoCodec::Tiff:
        return ".tif";
    case VideoCodec::Dpx:
        return ".dpx";
    case VideoCodec::Jpeg:
        return ".jpg";
    case VideoCodec::Targa:
        return ".tga";
    case VideoCodec::Exr:
        return ".exr";
    default:
        return ".png";
    }
}

std::optional<Container> containerFromName(std::string_view name) {
    for (int c = 0; c <= static_cast<int>(Container::Image); ++c) {
        if (containerName(static_cast<Container>(c)) == name) {
            return static_cast<Container>(c);
        }
    }
    return std::nullopt;
}

std::optional<VideoCodec> videoCodecFromName(std::string_view name) {
    for (int c = 0; c <= static_cast<int>(VideoCodec::CineForm); ++c) {
        if (videoCodecName(static_cast<VideoCodec>(c)) == name) {
            return static_cast<VideoCodec>(c);
        }
    }
    return std::nullopt;
}

bool isAudioOnly(Container c) {
    return c == Container::Wav || c == Container::Mp3 || c == Container::M4a || c == Container::Aiff;
}

std::string_view rangeModeName(RangeMode mode) {
    switch (mode) {
    case RangeMode::WholeSequence:
        return "whole";
    case RangeMode::InOut:
        return "inOut";
    case RangeMode::WorkArea:
        return "workArea";
    case RangeMode::Custom:
        return "custom";
    }
    return "whole";
}

std::optional<RangeMode> rangeModeFromName(std::string_view name) {
    for (int m = 0; m <= static_cast<int>(RangeMode::Custom); ++m) {
        if (rangeModeName(static_cast<RangeMode>(m)) == name) {
            return static_cast<RangeMode>(m);
        }
    }
    return std::nullopt;
}

bool hasAlpha(OutputPixelFormat f) {
    using F = OutputPixelFormat;
    return oneOf(f, {F::Yuva444p10, F::Rgba, F::Rgba64, F::Gbrap12, F::Rgbaf});
}

std::string_view pixelFormatName(OutputPixelFormat f) {
    switch (f) {
    case OutputPixelFormat::Yuv420p:
        return "yuv420p";
    case OutputPixelFormat::Yuv420p10:
        return "yuv420p10";
    case OutputPixelFormat::Yuv422p:
        return "yuv422p";
    case OutputPixelFormat::Yuv422p10:
        return "yuv422p10";
    case OutputPixelFormat::Pal8:
        return "pal8";
    case OutputPixelFormat::Yuv444p10:
        return "yuv444p10";
    case OutputPixelFormat::Yuva444p10:
        return "yuva444p10";
    case OutputPixelFormat::Rgb24:
        return "rgb24";
    case OutputPixelFormat::Rgba:
        return "rgba";
    case OutputPixelFormat::Rgb48:
        return "rgb48";
    case OutputPixelFormat::Rgba64:
        return "rgba64";
    case OutputPixelFormat::Gbrp10:
        return "gbrp10";
    case OutputPixelFormat::Gbrp12:
        return "gbrp12";
    case OutputPixelFormat::Gbrap12:
        return "gbrap12";
    case OutputPixelFormat::Rgbf:
        return "rgbf";
    case OutputPixelFormat::Rgbaf:
        return "rgbaf";
    }
    return "yuv420p";
}

bool isDnxhd(DnxhrProfile p) {
    return p == DnxhrProfile::DnxhdLb || p == DnxhrProfile::DnxhdSq || p == DnxhrProfile::DnxhdHq ||
           p == DnxhrProfile::DnxhdHqx;
}

std::optional<std::int64_t> dnxhdBitrate(DnxhrProfile profile, int width, int height, Rate rate) {
    if (!isDnxhd(profile) || width != 1920 || height != 1080 || !rate.isValid()) {
        return std::nullopt;
    }
    // Avid's 1080p families by frame rate (the bitrates this FFmpeg's DNxHD encoder accepts at 1920x1080).
    const double fps = rate.toDouble();
    int family = -1; // 0: 23.976/24, 1: 25, 2: 29.97/30, 3: 50, 4: 59.94/60
    if (std::abs(fps - 24.0) < 0.1) {
        family = 0;
    } else if (std::abs(fps - 25.0) < 0.01) {
        family = 1;
    } else if (std::abs(fps - 30.0) < 0.1) {
        family = 2;
    } else if (std::abs(fps - 50.0) < 0.01) {
        family = 3;
    } else if (std::abs(fps - 60.0) < 0.1) {
        family = 4;
    }
    if (family < 0) {
        return std::nullopt;
    }
    constexpr int kLb[] = {36, 36, 45, 75, 90};
    constexpr int kSq[] = {115, 120, 145, 240, 290};
    constexpr int kHq[] = {175, 185, 220, 365, 440};
    constexpr int kHqx[] = {175, 185, 0, 365, 440}; // 220x is not in this FFmpeg
    const auto f = static_cast<std::size_t>(family);
    const int mbps = profile == DnxhrProfile::DnxhdLb   ? kLb[f]
                     : profile == DnxhrProfile::DnxhdSq ? kSq[f]
                     : profile == DnxhrProfile::DnxhdHq ? kHq[f]
                                                        : kHqx[f];
    if (mbps == 0) {
        return std::nullopt;
    }
    return std::int64_t{mbps} * 1'000'000;
}

std::string_view videoCodecName(VideoCodec c) {
    switch (c) {
    case VideoCodec::H264:
        return "h264";
    case VideoCodec::Hevc:
        return "hevc";
    case VideoCodec::ProRes:
        return "prores";
    case VideoCodec::DnxHR:
        return "dnxhr";
    case VideoCodec::Av1:
        return "av1";
    case VideoCodec::Gif:
        return "gif";
    case VideoCodec::Vp9:
        return "vp9";
    case VideoCodec::Png:
        return "png";
    case VideoCodec::Tiff:
        return "tiff";
    case VideoCodec::Dpx:
        return "dpx";
    case VideoCodec::Jpeg:
        return "jpeg";
    case VideoCodec::Targa:
        return "targa";
    case VideoCodec::Exr:
        return "exr";
    case VideoCodec::Animation:
        return "animation";
    case VideoCodec::CineForm:
        return "cineform";
    }
    return "h264";
}

std::string_view dynamicRangeName(DynamicRange range) {
    switch (range) {
    case DynamicRange::Auto:
        return "auto";
    case DynamicRange::Sdr:
        return "sdr";
    case DynamicRange::Pq:
        return "pq";
    case DynamicRange::Hlg:
        return "hlg";
    }
    return "auto";
}

std::string_view audioCodecName(AudioCodec c) {
    switch (c) {
    case AudioCodec::Aac:
        return "aac";
    case AudioCodec::Pcm16:
        return "pcm_s16le";
    case AudioCodec::Pcm24:
        return "pcm_s24le";
    case AudioCodec::Ac3:
        return "ac3";
    case AudioCodec::Opus:
        return "libopus";
    case AudioCodec::Mp3:
        return "mp3_mf";
    }
    return "aac";
}

std::string_view audioCodecKey(AudioCodec c) {
    switch (c) {
    case AudioCodec::Aac:
        return "aac";
    case AudioCodec::Pcm16:
        return "pcm16";
    case AudioCodec::Pcm24:
        return "pcm24";
    case AudioCodec::Ac3:
        return "ac3";
    case AudioCodec::Opus:
        return "opus";
    case AudioCodec::Mp3:
        return "mp3";
    }
    return "aac";
}

std::optional<AudioCodec> audioCodecFromKey(std::string_view key) {
    for (int c = 0; c <= static_cast<int>(AudioCodec::Mp3); ++c) {
        if (audioCodecKey(static_cast<AudioCodec>(c)) == key) {
            return static_cast<AudioCodec>(c);
        }
    }
    return std::nullopt;
}

int exportAudioChannels(const AudioSettings& audio, ChannelFormat masterFormat) {
    if (audio.channels > 0) {
        return audio.channels;
    }
    // AAC, AC-3, Opus and PCM take 5.1; MP3 is stereo at most (a 5.1 master is downmixed).
    return masterFormat == ChannelFormat::Surround51 && audio.codec != AudioCodec::Mp3 ? 6 : 2;
}

} // namespace wabcut::exporter
