#include "Importers.h"
#include "TextUtil.h"

#include <limits>
#include <zlib.h>

namespace wabcut::interchange::detail {

std::expected<std::string, Error> gunzip(std::string_view data, std::size_t maxBytes, bool allowTruncated) {
    if (!hasGzipMagic(data)) {
        return std::unexpected(parseError("not gzip data"));
    }
    z_stream stream{};
    // 16 + MAX_WBITS: expect a gzip header and trailer (not a raw zlib stream).
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        return std::unexpected(Error{ErrorCode::Unknown, "zlib initialisation failed"});
    }
    std::string out;
    constexpr std::size_t kChunk = 256 * 1024;
    std::size_t consumed = 0;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0) {
            if (consumed >= data.size()) {
                break; // input ended before the stream did
            }
            const std::size_t take =
                std::min<std::size_t>(data.size() - consumed, std::numeric_limits<uInt>::max());
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + consumed));
            stream.avail_in = static_cast<uInt>(take);
            consumed += take;
        }
        const std::size_t before = out.size();
        const std::size_t room = std::min(kChunk, maxBytes - std::min(maxBytes, before));
        if (room == 0) {
            inflateEnd(&stream);
            if (allowTruncated) {
                return out;
            }
            return std::unexpected(
                parseError("decompressed data exceeds " + std::to_string(maxBytes) + " bytes"));
        }
        out.resize(before + room);
        stream.next_out = reinterpret_cast<Bytef*>(out.data() + before);
        stream.avail_out = static_cast<uInt>(room);
        status = inflate(&stream, Z_NO_FLUSH);
        out.resize(before + (room - stream.avail_out));
        if (status == Z_NEED_DICT || status == Z_DATA_ERROR || status == Z_MEM_ERROR) {
            inflateEnd(&stream);
            if (allowTruncated && !out.empty()) {
                return out;
            }
            return std::unexpected(parseError("corrupt gzip data"));
        }
        if (status == Z_BUF_ERROR && stream.avail_in == 0 && consumed >= data.size()) {
            break;
        }
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END && !allowTruncated) {
        return std::unexpected(parseError("gzip data is truncated"));
    }
    return out;
}

} // namespace wabcut::interchange::detail
