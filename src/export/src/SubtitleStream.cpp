#include "SubtitleStream.h"

#include <wabcut/export/CaptionSidecar.h>
#include <wabcut/playback/video/CaptionRedaction.h>

extern "C" {
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace wabcut::exporter::detail {

namespace {

// FFmpeg's default ASS header: the subtitle encoders read their default style from it (mov_text builds its sample
// description from it). Cue packets are written directly below, so no ASS markup ever reaches a player.
constexpr const char* kAssHeader =
    "[Script Info]\r\n"
    "ScriptType: v4.00+\r\n"
    "PlayResX: 384\r\n"
    "PlayResY: 288\r\n"
    "ScaledBorderAndShadow: yes\r\n"
    "\r\n"
    "[V4+ Styles]\r\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
    "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
    "MarginV, Encoding\r\n"
    "Style: Default,Arial,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\r\n"
    "\r\n"
    "[Events]\r\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n";

std::int64_t toMs(Ticks t) {
    return static_cast<std::int64_t>(static_cast<long double>(t.value) * 1000.0L / kTicksPerSecond + 0.5L);
}

/// The cue's lines (CR LF, CR and LF end a line), blank ones dropped, joined with '\n'.
std::string plainText(std::string_view text) {
    std::string out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        start = end + (end + 1 < text.size() && text[end] == '\r' && text[end + 1] == '\n' ? 2 : 1);
        if (line.find_first_not_of(" \t") == std::string_view::npos) {
            continue;
        }
        if (!out.empty()) {
            out += '\n';
        }
        out += line;
    }
    return out;
}

} // namespace

std::string subtitleLanguage(const Sequence& sequence, const ExportSettings& settings) {
    if (!settings.subtitles.language.empty()) {
        return settings.subtitles.language;
    }
    static constexpr std::pair<std::string_view, std::string_view> kCodes[] = {
        {"en", "eng"}, {"th", "tha"}, {"ja", "jpn"}, {"zh", "zho"}, {"ko", "kor"}, {"es", "spa"}, {"fr", "fra"},
        {"de", "deu"}, {"it", "ita"}, {"pt", "por"}, {"ru", "rus"}, {"vi", "vie"}, {"id", "ind"}, {"ar", "ara"},
        {"hi", "hin"}, {"nl", "nld"}, {"pl", "pol"}, {"tr", "tur"}, {"sv", "swe"}, {"uk", "ukr"}};
    if (const CaptionTrack* track = captionTrackToExport(sequence, settings.captions)) {
        const std::string_view lang = std::string_view(track->language).substr(0, 2);
        for (const auto& [two, three] : kCodes) {
            if (two == lang) {
                return std::string(three);
            }
        }
    }
    return "und";
}

std::vector<SubtitleCue> subtitleCues(const ProjectState& project, const Sequence& sequence,
                                      const CaptionExportSettings& captions, TimeRange range) {
    std::vector<SubtitleCue> out;
    const CaptionTrack* track = captionTrackToExport(sequence, captions);
    if (!track) {
        return out;
    }
    // Words the soundtrack bleeps or mutes are not written either (as in sidecars, Codex wave-4 P1-3).
    const CaptionTrack clean = playback::redactCaptionTrack(*track, playback::redactedSpeechRanges(project, sequence),
                                                            captions.words.get(), captions.redactedWord);
    for (const CaptionCue& cue : clean.cues) {
        const Ticks start = std::max(cue.start, range.start);
        const Ticks end = std::min(cue.end(), range.end());
        std::string text = plainText(cue.text);
        if (!(start < end) || text.empty()) {
            continue;
        }
        if (text.size() > 60000) { // a 3GPP text sample holds at most 65535 bytes
            text.resize(60000);
        }
        SubtitleCue c{toMs(start - range.start), toMs(end - range.start), std::move(text)};
        if (c.endMs > c.startMs) {
            out.push_back(std::move(c));
        }
    }
    return out;
}

std::expected<SubtitleStream, Error> SubtitleStream::add(Container container, Muxer& muxer, const std::string& language) {
    const AVCodecID id = container == Container::Mkv ? AV_CODEC_ID_SUBRIP : AV_CODEC_ID_MOV_TEXT;
    const AVCodec* codec = avcodec_find_encoder(id);
    if (!codec) {
        return std::unexpected(Error{ErrorCode::Unsupported, "No subtitle encoder for this format in this FFmpeg build"});
    }
    SubtitleStream s;
    s.m_ctx.reset(avcodec_alloc_context3(codec));
    if (!s.m_ctx) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (subtitle encoder)"});
    }
    AVCodecContext* ctx = s.m_ctx.get();
    ctx->time_base = AVRational{1, 1000};
    const std::size_t headerSize = std::strlen(kAssHeader);
    ctx->subtitle_header = static_cast<std::uint8_t*>(av_mallocz(headerSize + 1));
    if (!ctx->subtitle_header) {
        return std::unexpected(Error{ErrorCode::EncoderError, "Out of memory (subtitle header)"});
    }
    std::memcpy(ctx->subtitle_header, kAssHeader, headerSize);
    ctx->subtitle_header_size = static_cast<int>(headerSize);
    if (muxer.wantsGlobalHeader()) {
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (const int r = avcodec_open2(ctx, codec, nullptr); r < 0) {
        return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot open the subtitle encoder", r));
    }
    auto stream = muxer.addStream(ctx);
    if (!stream) {
        return std::unexpected(stream.error());
    }
    s.m_stream = *stream;
    Muxer::setLanguage(s.m_stream, language.empty() ? std::string("und") : language);
    return s;
}

std::expected<void, Error> SubtitleStream::write(const std::vector<SubtitleCue>& cues, Muxer& muxer) {
    const bool movText = m_ctx->codec_id == AV_CODEC_ID_MOV_TEXT;
    PacketPtr packet(av_packet_alloc());
    for (const SubtitleCue& cue : cues) {
        // 3GPP Timed Text: a 16-bit big-endian length, then the UTF-8 text; SubRip in Matroska: the text itself.
        const std::size_t size = cue.text.size() + (movText ? 2 : 0);
        if (const int r = av_new_packet(packet.get(), static_cast<int>(size)); r < 0) {
            return std::unexpected(ffmpegError(ErrorCode::EncoderError, "Cannot allocate a subtitle", r));
        }
        std::uint8_t* data = packet->data;
        if (movText) {
            data[0] = static_cast<std::uint8_t>((cue.text.size() >> 8) & 0xFF);
            data[1] = static_cast<std::uint8_t>(cue.text.size() & 0xFF);
            data += 2;
        }
        std::memcpy(data, cue.text.data(), cue.text.size());
        packet->pts = cue.startMs;
        packet->dts = cue.startMs;
        packet->duration = cue.endMs - cue.startMs;
        packet->flags |= AV_PKT_FLAG_KEY;
        if (auto ok = muxer.write(packet.get(), m_ctx->time_base, m_stream); !ok) {
            return ok;
        }
        av_packet_unref(packet.get());
    }
    return {};
}

} // namespace wabcut::exporter::detail
