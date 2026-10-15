// Stream descriptions shared by probe() and the decoders: colour tags, exact frame rate / VFR detection,
// rotation, audio stream titles.

#include "FfmpegUtil.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace wabcut::media::detail {

namespace {

ColorMatrix toMatrix(AVColorSpace s) {
    switch (s) {
    case AVCOL_SPC_BT709:
        return ColorMatrix::BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_FCC:
        return ColorMatrix::BT601;
    case AVCOL_SPC_BT2020_NCL:
        return ColorMatrix::BT2020NonConstant;
    case AVCOL_SPC_BT2020_CL:
        return ColorMatrix::BT2020Constant;
    case AVCOL_SPC_SMPTE240M:
        return ColorMatrix::SMPTE240M;
    case AVCOL_SPC_RGB:
        return ColorMatrix::RGB;
    default:
        return ColorMatrix::Unknown;
    }
}

ColorRange toRange(AVColorRange r) {
    switch (r) {
    case AVCOL_RANGE_MPEG:
        return ColorRange::Limited;
    case AVCOL_RANGE_JPEG:
        return ColorRange::Full;
    default:
        return ColorRange::Unknown;
    }
}

ColorPrimaries toPrimaries(AVColorPrimaries p) {
    switch (p) {
    case AVCOL_PRI_BT709:
        return ColorPrimaries::BT709;
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M:
        return ColorPrimaries::BT601_525;
    case AVCOL_PRI_BT470BG:
        return ColorPrimaries::BT601_625;
    case AVCOL_PRI_BT2020:
        return ColorPrimaries::BT2020;
    case AVCOL_PRI_SMPTE431:
        return ColorPrimaries::DCIP3;
    case AVCOL_PRI_SMPTE432:
        return ColorPrimaries::DisplayP3;
    default:
        return ColorPrimaries::Unknown;
    }
}

TransferFunction toTransfer(AVColorTransferCharacteristic t) {
    switch (t) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
        return TransferFunction::BT709; // same OETF family
    case AVCOL_TRC_IEC61966_2_1:
        return TransferFunction::SRGB;
    case AVCOL_TRC_LINEAR:
        return TransferFunction::Linear;
    case AVCOL_TRC_GAMMA22:
        return TransferFunction::Gamma22;
    case AVCOL_TRC_GAMMA28:
        return TransferFunction::Gamma28;
    case AVCOL_TRC_SMPTE2084:
        return TransferFunction::PQ;
    case AVCOL_TRC_ARIB_STD_B67:
        return TransferFunction::HLG;
    default:
        return TransferFunction::Unknown;
    }
}

bool validRational(AVRational r) {
    return r.num > 0 && r.den > 0;
}

/// Broadcast and camera rates a measured rate snaps to.
constexpr std::array kStandardRates{
    Rate{24000, 1001}, Rate{24, 1},  Rate{25, 1},       Rate{30000, 1001}, Rate{30, 1},  Rate{48000, 1001},
    Rate{48, 1},       Rate{50, 1},  Rate{60000, 1001}, Rate{60, 1},       Rate{100, 1}, Rate{120000, 1001},
    Rate{120, 1},      Rate{144, 1}, Rate{240, 1},      Rate{12, 1},       Rate{15, 1},  Rate{16, 1},
    Rate{18, 1},       Rate{20, 1},  Rate{72, 1},       Rate{90, 1},       Rate{96, 1},  Rate{180, 1},
};

std::optional<Rate> snapToStandard(double fps) {
    if (!(fps > 0.0)) {
        return std::nullopt;
    }
    for (const Rate r : kStandardRates) {
        if (std::abs(fps / r.toDouble() - 1.0) < 2e-4) {
            return r;
        }
    }
    return std::nullopt;
}

double toDouble(AVRational r) {
    return validRational(r) ? av_q2d(r) : 0.0;
}

Rate toRate(AVRational r) {
    AVRational reduced{};
    av_reduce(&reduced.num, &reduced.den, r.num, r.den, 1'000'000);
    return Rate{reduced.num, reduced.den};
}

struct RateGuess {
    Rate rate{};
    bool variable = false;
};

/// Frame-duration statistics from the demuxer's sample index. Only trusted when the index lists (nearly)
/// every frame of the stream, which is the case for MP4/MOV; Matroska cues only list keyframes.
struct IndexTiming {
    double meanDelta = 0.0; // in stream time base units
    bool uniform = true;
};

std::optional<IndexTiming> indexTiming(AVStream* st) {
    const int n = avformat_index_get_entries_count(st);
    if (n < 3 || st->nb_frames <= 0 || n + 1 < st->nb_frames) {
        return std::nullopt;
    }
    std::vector<std::int64_t> ts;
    ts.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const AVIndexEntry* e = avformat_index_get_entry(st, i);
        if (e && e->timestamp != AV_NOPTS_VALUE) {
            ts.push_back(e->timestamp);
        }
    }
    if (ts.size() < 3) {
        return std::nullopt;
    }
    std::sort(ts.begin(), ts.end());
    const double span = static_cast<double>(ts.back() - ts.front());
    if (span <= 0.0) {
        return std::nullopt;
    }
    IndexTiming timing;
    timing.meanDelta = span / static_cast<double>(ts.size() - 1);
    // Allow one time-base unit of rounding jitter (e.g. 29.97 fps in a 1 ms time base alternates 33/34).
    const double tolerance = std::max(1.0, timing.meanDelta * 0.005);
    for (std::size_t i = 1; i < ts.size(); ++i) {
        const double delta = static_cast<double>(ts[i] - ts[i - 1]);
        if (std::abs(delta - timing.meanDelta) > tolerance) {
            timing.uniform = false;
            break;
        }
    }
    return timing;
}

RateGuess guessFrameRate(AVStream* st) {
    const AVRational avg = st->avg_frame_rate;
    const AVRational real = st->r_frame_rate;
    const AVRational coded = st->codecpar->framerate;

    RateGuess guess;
    if (const auto timing = indexTiming(st)) {
        guess.variable = !timing->uniform;
        if (timing->uniform) {
            const double fps = 1.0 / (timing->meanDelta * av_q2d(st->time_base));
            if (auto snapped = snapToStandard(fps)) {
                guess.rate = *snapped;
                return guess;
            }
            if (validRational(avg) && std::abs(toDouble(avg) / fps - 1.0) < 1e-3) {
                guess.rate = toRate(avg);
                return guess;
            }
            guess.rate = toRate(av_d2q(fps, 1'000'000));
            return guess;
        }
    } else if (validRational(avg) && validRational(real)) {
        // No usable index: FFmpeg's real base rate is the LCM of all frame durations, so it disagrees with
        // the average for VFR content. A 2x ratio is field-rate signalling of interlaced CFR content, not
        // VFR.
        const double ratio = toDouble(real) / toDouble(avg);
        guess.variable = std::abs(ratio - 1.0) > 0.01 && std::abs(ratio - 2.0) > 0.01;
    }

    for (const AVRational candidate : {avg, real, coded}) {
        if (auto snapped = snapToStandard(toDouble(candidate))) {
            guess.rate = *snapped;
            return guess;
        }
    }
    for (const AVRational candidate : {avg, real, coded}) {
        if (validRational(candidate)) {
            guess.rate = toRate(candidate);
            return guess;
        }
    }
    guess.rate = Rate{};
    return guess;
}

ChromaLocation toChromaLocation(AVChromaLocation l) {
    switch (l) {
    case AVCHROMA_LOC_LEFT:
        return ChromaLocation::Left;
    case AVCHROMA_LOC_CENTER:
        return ChromaLocation::Center;
    case AVCHROMA_LOC_TOPLEFT:
        return ChromaLocation::TopLeft;
    case AVCHROMA_LOC_TOP:
        return ChromaLocation::Top;
    case AVCHROMA_LOC_BOTTOMLEFT:
        return ChromaLocation::BottomLeft;
    case AVCHROMA_LOC_BOTTOM:
        return ChromaLocation::Bottom;
    default:
        return ChromaLocation::Unknown;
    }
}

std::string chromaName(const AVPixFmtDescriptor* desc) {
    if (!desc) {
        return {};
    }
    if (desc->nb_components < 3) {
        return "4:0:0";
    }
    if ((desc->flags & AV_PIX_FMT_FLAG_RGB) != 0) {
        return "4:4:4";
    }
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
        return "4:2:0";
    }
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
        return "4:2:2";
    }
    if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) {
        return "4:4:4";
    }
    if (desc->log2_chroma_w == 2 && desc->log2_chroma_h == 0) {
        return "4:1:1";
    }
    if (desc->log2_chroma_w == 2 && desc->log2_chroma_h == 2) {
        return "4:1:0";
    }
    return {};
}

Ticks streamDuration(const AVFormatContext* fmt, const AVStream* st) {
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
        if (const auto d = toTicks(st->duration, st->time_base)) {
            return *d;
        }
    }
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        return toTicks(fmt->duration, AVRational{1, AV_TIME_BASE}).value_or(Ticks{});
    }
    return Ticks{0};
}

// ---- Audio stream titles (A.10: multi-track ingest)

/// The names muxers write into an audio track's handler (MP4 `hdlr`) or title when nobody named the track. They say
/// nothing about what the track carries, so a stream with one of them reads as untitled. Compared ignoring ASCII case.
/// This is the one list of them: add new ones here.
constexpr std::string_view kGenericAudioNames[] = {
    "SoundHandler",                        // FFmpeg (movenc's default), so OBS and most tools
    "SoundHandle",                         // Android's MediaRecorder
    "Sound Handler",
    "Sound Media Handler",
    "Apple Sound Media Handler",           // QuickTime
    "Core Media Audio",                    // iPhone, macOS (AVFoundation)
    "DataHandler",                         // QuickTime's data handler (FFmpeg's name), when there is no media handler
    "Alias Data Handler",
    "Apple Alias Data Handler",
    "AudioHandler",
    "Audio Handler",
    "GPAC ISO Audio Handler",              // MP4Box
    "L-SMASH Audio Handler",               // L-SMASH (x264/x265 toolchains)
    "Mainconcept MP4 Sound Media Handler", // MainConcept (Premiere, Media Encoder)
    "Bento4 Sound Handler",                // Bento4
    "GoPro AAC",                           // GoPro cameras
    "Stereo",                              // HandBrake and others name a track after its mixdown
    "Mono",
};
/// Generic names a tool continues with details (YouTube: "... Created on: <date>.").
constexpr std::string_view kGenericAudioNamePrefixes[] = {"ISO Media file produced by Google Inc."};

/// A stored title is bounded: files are untrusted and the title is saved with the project.
constexpr std::size_t kMaxTitleBytes = 256;

char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lowerAscii(text[i]) != lowerAscii(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool isGenericAudioName(std::string_view name) {
    for (const std::string_view generic : kGenericAudioNames) {
        if (name.size() == generic.size() && startsWithIgnoringCase(name, generic)) {
            return true;
        }
    }
    for (const std::string_view prefix : kGenericAudioNamePrefixes) {
        if (startsWithIgnoringCase(name, prefix)) {
            return true;
        }
    }
    return false;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// Well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF), as the project file's JSON writer
/// requires. The same rule as edit::isValidUtf8 (TitleOps.h), kept here so media does not depend on the edit headers.
bool isUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80u) {
            ++i;
            continue;
        }
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0u) == 0xC0u) {
            n = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {
            n = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {
            n = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        for (std::size_t k = 1; k <= n; ++k) {
            if (i + k >= s.size()) {
                return false;
            }
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        constexpr std::uint32_t kMinimum[] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMinimum[n] || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

/// `text` without leading and trailing whitespace, cut to at most kMaxTitleBytes on a UTF-8 character boundary.
std::string_view trimmedTitle(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    if (text.size() > kMaxTitleBytes) {
        std::size_t end = kMaxTitleBytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
            --end; // text[end] continues a character: cut before that character
        }
        text = text.substr(0, end);
        while (!text.empty() && isSpace(text.back())) {
            text.remove_suffix(1);
        }
    }
    return text;
}

/// The track name the file gives the stream: its title (FFmpeg maps the Matroska TrackName there), else its handler
/// name (MP4 `hdlr`, where OBS's hybrid and native MP4 put the track name). Generic names and text that is not UTF-8
/// (a title in a legacy code page would make the project unsavable) read as no name.
std::string audioStreamTitle(const AVStream* st) {
    for (const char* key : {"title", "handler_name"}) {
        const AVDictionaryEntry* entry = av_dict_get(st->metadata, key, nullptr, 0); // keys ignore case
        if (!entry || !entry->value) {
            continue;
        }
        const std::string_view text = trimmedTitle(entry->value);
        if (!text.empty() && !isGenericAudioName(text) && isUtf8(text)) {
            return std::string(text);
        }
    }
    return {};
}

} // namespace

ColorInfo colorInfoFrom(AVColorSpace space, AVColorRange range, AVColorPrimaries primaries,
                        AVColorTransferCharacteristic transfer, AVChromaLocation chroma) {
    return ColorInfo{toMatrix(space), toRange(range), toPrimaries(primaries), toTransfer(transfer),
                     toChromaLocation(chroma)};
}

Orientation orientationOf(const AVCodecParameters* par) {
    const AVPacketSideData* sd =
        av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < static_cast<std::size_t>(9 * sizeof(std::int32_t))) {
        return {};
    }
    std::int32_t m[9];
    std::memcpy(m, sd->data, sizeof m);
    Orientation o;
    // A reflection has a negative determinant. Undo it (FFmpeg flips the matrix's output x), then read the
    // angle.
    o.mirrored = static_cast<std::int64_t>(m[0]) * m[4] - static_cast<std::int64_t>(m[1]) * m[3] < 0;
    if (o.mirrored) {
        av_display_matrix_flip(m, 1, 0);
    }
    // av_display_rotation_get() returns the counter-clockwise angle the matrix rotates by; displaying the
    // picture upright needs the opposite (clockwise) rotation. Round to a multiple of 90.
    const double ccw = av_display_rotation_get(m);
    if (std::isnan(ccw)) {
        return {};
    }
    const long quarter = std::lround(-ccw / 90.0);
    o.rotationDegrees = static_cast<std::int16_t>(((quarter % 4) + 4) % 4 * 90);
    return o;
}

VideoStreamInfo describeVideoStream(AVFormatContext* fmt, AVStream* st) {
    const AVCodecParameters* par = st->codecpar;
    VideoStreamInfo info;
    info.index = st->index;
    info.codec = avcodec_get_name(par->codec_id);
    if (const char* profile = avcodec_profile_name(par->codec_id, par->profile)) {
        info.profile = profile;
    }
    info.width = par->width;
    info.height = par->height;

    const RateGuess rate = guessFrameRate(st);
    info.frameRate = rate.rate;
    info.variableFrameRate = rate.variable;

    AVRational sar = st->sample_aspect_ratio;
    if (!validRational(sar)) {
        sar = par->sample_aspect_ratio;
    }
    if (validRational(sar)) {
        info.sampleAspect = toRate(sar);
    }

    const auto pixFmt = static_cast<AVPixelFormat>(par->format);
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt)) {
        info.pixelFormat = desc->name;
        info.bitDepth = desc->comp[0].depth;
        info.chromaSubsampling = chromaName(desc);
    }
    info.color = colorInfoFrom(par->color_space, par->color_range, par->color_primaries, par->color_trc,
                               par->chroma_location);
    info.rotationDegrees = orientationOf(par).rotationDegrees;
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);
        desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0) {
        // Codecs with alpha (ProRes 4444, PNG, VP9/HEVC alpha) are straight unless the stream says otherwise.
        info.alpha =
            par->alpha_mode == AVALPHA_MODE_PREMULTIPLIED ? AlphaMode::Premultiplied : AlphaMode::Straight;
    }
    info.duration = streamDuration(fmt, st);

    if (st->nb_frames > 0) {
        info.frameCount = st->nb_frames;
    } else if (!info.variableFrameRate && info.frameRate.isValid() && info.duration.value > 0) {
        // Matroska and friends do not store a count; derive it from duration at the nominal rate.
        info.frameCount = ticksToFrames(info.duration, info.frameRate, Rounding::Nearest).value_or(-1);
    }
    return info;
}

AudioStreamInfo describeAudioStream(const AVFormatContext* fmt, const AVStream* st) {
    const AVCodecParameters* par = st->codecpar;
    AudioStreamInfo info;
    info.index = st->index;
    info.codec = avcodec_get_name(par->codec_id);
    info.sampleRate = Rate{par->sample_rate, 1};
    info.channels = par->ch_layout.nb_channels;
    char layout[128] = {};
    if (par->ch_layout.nb_channels > 0 &&
        av_channel_layout_describe(&par->ch_layout, layout, sizeof layout) > 0) {
        info.channelLayout = layout;
    }
    info.duration = streamDuration(fmt, st);
    info.title = audioStreamTitle(st);
    return info;
}

} // namespace wabcut::media::detail
