// The demuxer's metadata tags (ContainerTags.h, 0.2 P.3).

#include "FfmpegUtil.h"

#include <wabcut/media/ContainerTags.h>
#include <wabcut/model/MediaLocator.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <filesystem>
#include <system_error>

namespace wabcut::media {

using detail::ensureFfmpegLogging;
using detail::ffmpegError;
using detail::FormatContextPtr;

namespace {

/// `text` cut to at most `limit` bytes without splitting a UTF-8 sequence.
std::string bounded(const char* text, std::size_t limit) {
    std::string out = text ? text : "";
    if (out.size() <= limit) {
        return out;
    }
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    out.resize(cut);
    return out;
}

void collect(const AVDictionary* dict, int stream, const std::string& kind, std::vector<ContainerTag>& out) {
    const AVDictionaryEntry* entry = nullptr;
    for (std::size_t i = 0; i < kMaxContainerTags && (entry = av_dict_iterate(dict, entry)) != nullptr; ++i) {
        out.push_back(ContainerTag{stream, kind, bounded(entry->key, 256), bounded(entry->value, kMaxContainerTagBytes)});
    }
}

} // namespace

std::expected<std::vector<ContainerTag>, Error> readContainerTags(const std::string& path) {
    ensureFfmpegLogging();
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(pathFromUtf8(path), ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "File not found: " + path});
    }
    AVFormatContext* raw = nullptr;
    const int r = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (r < 0) {
        return std::unexpected(ffmpegError(r == AVERROR(ENOENT) ? ErrorCode::NotFound : ErrorCode::Unsupported,
                                           "Cannot open " + path, r));
    }
    FormatContextPtr fmt(raw);
    std::vector<ContainerTag> out;
    collect(fmt->metadata, -1, {}, out);
    for (unsigned i = 0; i < fmt->nb_streams && i < 64; ++i) {
        const AVStream* s = fmt->streams[i];
        const AVMediaType type = s->codecpar ? s->codecpar->codec_type : AVMEDIA_TYPE_UNKNOWN;
        const std::string kind = type == AVMEDIA_TYPE_VIDEO      ? "video"
                                 : type == AVMEDIA_TYPE_AUDIO    ? "audio"
                                 : type == AVMEDIA_TYPE_SUBTITLE ? "subtitle"
                                                                 : "data";
        collect(s->metadata, static_cast<int>(i), kind, out);
    }
    return out;
}

} // namespace wabcut::media
