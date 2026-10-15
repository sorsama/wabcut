#include <wabcut/export/ExportSettings.h>

#include <algorithm>
#include <cmath>
#include <format>

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
    }
    return "none";
}

namespace {

Error invalid(std::string message) {
    return Error{ErrorCode::InvalidArgument, std::move(message)};
}

bool is422(OutputPixelFormat f) {
    return f == OutputPixelFormat::Yuv422p || f == OutputPixelFormat::Yuv422p10;
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
        switch (v.codec) {
        case VideoCodec::H264:
            break;
        case VideoCodec::Hevc:
            if (v.pixelFormat == OutputPixelFormat::Yuv422p10) {
                return std::unexpected(invalid("HEVC export supports 4:2:0 8/10-bit and 4:2:2 8-bit"));
            }
            break;
        case VideoCodec::ProRes:
            if (v.pixelFormat != OutputPixelFormat::Yuv422p10) {
                return std::unexpected(invalid("ProRes 422 is 4:2:2 10-bit (yuv422p10)"));
            }
            break;
        case VideoCodec::DnxHR:
            if ((v.dnxhrProfile == DnxhrProfile::Hqx) != (v.pixelFormat == OutputPixelFormat::Yuv422p10) ||
                !is422(v.pixelFormat)) {
                return std::unexpected(
                    invalid("DNxHR LB/SQ/HQ are 4:2:2 8-bit (yuv422p), DNxHR HQX is 4:2:2 10-bit"));
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
    }
    if (s.range == RangeMode::Custom &&
        (!s.customRange || s.customRange->isEmpty() || s.customRange->start.value < 0)) {
        return std::unexpected(invalid("A custom export range must be set, non-empty and not negative"));
    }
    return {};
}

std::expected<ResolvedExport, Error> resolve(const ExportSettings& s, const Sequence& sequence) {
    if (auto ok = validate(s); !ok) {
        return std::unexpected(ok.error());
    }
    ResolvedExport r;
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
        r.frameRate = s.video.frameRate.value_or(sequence.frameRate);
        const auto frames = ticksToFrames(r.range.duration, r.frameRate, Rounding::Ceil);
        const auto duration = frames ? framesToTicks(*frames, r.frameRate, Rounding::Nearest) : std::nullopt;
        if (!frames || !duration) {
            return std::unexpected(invalid("The export range is too long for its frame rate"));
        }
        r.frameCount = *frames;
        r.duration = *duration;
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
    }
    return ".mp4";
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
    }
    return "h264";
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
    }
    return "aac";
}

int exportAudioChannels(const AudioSettings& audio, ChannelFormat masterFormat) {
    if (audio.channels > 0) {
        return audio.channels;
    }
    // Every codec here takes 5.1 (AAC, AC-3, PCM).
    return masterFormat == ChannelFormat::Surround51 ? 6 : 2;
}

} // namespace wabcut::exporter
