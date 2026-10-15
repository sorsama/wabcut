#pragma once

// Format parsers: bytes -> IrDocument. Each one validates its input completely; malformed or hostile
// content yields an Error (ParseError), never a crash, an unbounded allocation or deep recursion.

#include "Ir.h"

#include <wabcut/interchange/Import.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace wabcut::interchange::detail {

struct ParseContext {
    /// File label for report locations.
    std::string fileLabel;
    const ImportOptions* options = nullptr;
};

/// Nesting limit for recursive structures (nested sequences, stacks, compound clips).
inline constexpr int kMaxNesting = 32;

std::expected<IrDocument, Error> parseFcp7Xml(std::string_view data, const ParseContext& context);
std::expected<IrDocument, Error> parseFcpXml(std::string_view data, const ParseContext& context);
std::expected<IrDocument, Error> parseOtio(std::string_view data, const ParseContext& context);
std::expected<IrDocument, Error> parseEdl(std::string_view data, const ParseContext& context);
/// `data` is the .prproj file as stored (gzip-compressed XML, or plain XML).
std::expected<IrDocument, Error> parsePremiereProject(std::string_view data, const ParseContext& context);

/// Inflates gzip data, refusing to produce more than `maxBytes` (decompression bombs). With
/// `allowTruncated`, returns what decompressed before the input ended (for sniffing a file prefix).
std::expected<std::string, Error> gunzip(std::string_view data, std::size_t maxBytes,
                                         bool allowTruncated = false);

inline bool hasGzipMagic(std::string_view data) {
    return data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0x1F &&
           static_cast<unsigned char>(data[1]) == 0x8B;
}

} // namespace wabcut::interchange::detail
