// A recording's chapter markers (Chapters.h, P0 round: OBS chapter markers on import).

#include "FfmpegUtil.h"

#include <wabcut/media/Chapters.h>
#include <wabcut/model/MediaLocator.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <filesystem>
#include <string_view>
#include <system_error>

namespace wabcut::media {

using detail::ensureFfmpegLogging;
using detail::ffmpegError;
using detail::FormatContextPtr;
using detail::toTicks;

namespace {

/// Length of the valid UTF-8 sequence at `text[at]` (1..4), or 0 when the bytes there are not one.
std::size_t utf8Length(std::string_view text, std::size_t at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    std::size_t length = 0;
    char32_t value = 0;
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
    } else {
        return 0;
    }
    if (at + length > text.size()) {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(at + i) & 0xC0) != 0x80) {
            return 0;
        }
        value = (value << 6) | (byte(at + i) & 0x3F);
    }
    // Overlong forms, UTF-16 surrogates and values past U+10FFFF are not UTF-8.
    constexpr char32_t kSmallest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < kSmallest[length] || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) {
        return 0;
    }
    return length;
}

} // namespace

std::string cleanChapterTitle(std::string_view text) {
    // Valid UTF-8 (U+FFFD for bytes that are not), control characters as spaces, on one line, trimmed, at most
    // kMaxChapterTitleBytes without splitting a character.
    std::string out;
    // Reading stops a little past the bound (a hostile file may hold megabytes in one title); leading spaces and
    // control characters are skipped before anything is kept, so they never use up the bound.
    for (std::size_t at = 0; at < text.size() && out.size() < kMaxChapterTitleBytes + 8;) {
        const std::size_t length = utf8Length(text, at);
        if (length == 0) {
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++at;
            continue;
        }
        const auto c = static_cast<unsigned char>(text[at]);
        if (length == 1 && (c <= 0x20 || c == 0x7F)) {
            if (!out.empty()) {
                out += ' ';
            }
        } else {
            out.append(text.substr(at, length));
        }
        at += length;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.size() > kMaxChapterTitleBytes) {
        std::size_t cut = kMaxChapterTitleBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
    }
    return out;
}

std::expected<std::vector<MediaChapter>, Error> readChapters(const std::string& path) {
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
    std::vector<MediaChapter> out;
    for (unsigned i = 0; i < fmt->nb_chapters && out.size() < kMaxChapters; ++i) {
        const AVChapter* chapter = fmt->chapters[i];
        if (!chapter || chapter->time_base.num <= 0 || chapter->time_base.den <= 0) {
            continue;
        }
        const std::optional<Ticks> start = toTicks(chapter->start, chapter->time_base);
        if (!start || start->value < 0) {
            continue;
        }
        const std::optional<Ticks> end = toTicks(chapter->end, chapter->time_base);
        MediaChapter c;
        c.start = *start;
        c.end = end && *end >= *start ? *end : *start;
        if (const AVDictionaryEntry* e = av_dict_get(chapter->metadata, "title", nullptr, 0); e) {
            c.title = cleanChapterTitle(e->value ? std::string_view(e->value) : std::string_view());
        }
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace wabcut::media
