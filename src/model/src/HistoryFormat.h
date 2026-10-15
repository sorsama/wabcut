#pragma once

// The version history's files (History.cpp) as functions on bytes: index.json and the objects are untrusted input when
// a history folder is opened, and the fuzz harnesses (tests/fuzz) feed them through these directly.

#include <wabcut/model/Error.h>
#include <wabcut/model/History.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::detail {

struct HistoryIndex {
    std::vector<HistoryRevision> revisions;
    std::uint64_t nextRevision = 1;
};

/// index.json (bounded and checked: kMaxHistoryRevisions, keys, labels, increasing ids).
std::expected<HistoryIndex, Error> parseHistoryIndex(std::string_view text);

/// An object file: "WCH1", the text's length (8 bytes, little-endian), the zlib-compressed text.
std::expected<std::string, Error> encodeHistoryObject(std::string_view text);
/// The text of an object file. The length in its header bounds the text but is never allocated up front.
std::expected<std::string, Error> decodeHistoryObject(std::string_view file);

} // namespace wabcut::detail
