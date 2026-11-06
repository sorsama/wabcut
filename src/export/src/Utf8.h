#pragma once

// Private (0.7, #151): byte-count limits that never cut a UTF-8 character in two (Thai and CJK text is 3 bytes a
// character; a cut in the middle of one makes the whole string invalid UTF-8).

#include <cstddef>
#include <string>

namespace wabcut::exporter::detail {

/// Cuts `text` to at most `maxBytes` bytes, backing up to the start of the character the limit falls in.
inline void truncateUtf8(std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut; // a continuation byte: the character starts earlier
    }
    text.resize(cut);
}

} // namespace wabcut::exporter::detail
