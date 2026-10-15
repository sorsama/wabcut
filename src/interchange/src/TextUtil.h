#pragma once

// Small string, number and file helpers shared by the importers. Parsing is strict (no locale, no
// exceptions) because every input comes from an untrusted file.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail {

/// Largest file an importer reads (timelines are text; anything bigger is not a timeline).
inline constexpr std::size_t kMaxInputBytes = 256u * 1024u * 1024u;

std::string_view trim(std::string_view text);
bool iequals(std::string_view a, std::string_view b);
bool istartsWith(std::string_view text, std::string_view prefix);
bool iendsWith(std::string_view text, std::string_view suffix);
std::string toLowerAscii(std::string_view text);

/// Decimal integer with optional sign and surrounding blanks; nullopt on anything else or overflow.
std::optional<std::int64_t> parseInt64(std::string_view text);
/// Decimal floating-point number (no hex, no inf/nan); nullopt on anything else.
std::optional<double> parseDouble(std::string_view text);
/// TRUE/FALSE, true/false, 1/0, yes/no (case-insensitive).
std::optional<bool> parseBool(std::string_view text);

/// Splits on '\n', dropping a trailing '\r' from each line.
std::vector<std::string_view> splitLines(std::string_view text);
/// Splits on runs of blanks (space, tab).
std::vector<std::string_view> splitWords(std::string_view text);

/// Whether `text` is well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF).
bool isValidUtf8(std::string_view text);
/// `text` as UTF-8: well-formed UTF-8 sequences stay, every other byte is read as Windows-1252 (Latin-1 plus the
/// typographic punctuation of 0x80-0x9F; its five unassigned bytes become U+FFFD) - what EDLs and XML from older
/// Windows editors are written in. Imported text must be UTF-8, or the project could not be saved (C.10).
std::string toUtf8Lenient(std::string_view text);

/// UTF-8 <-> std::filesystem::path without the platform's narrow code page.
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string utf8FromPath(const std::filesystem::path& path);

/// Reads a whole file (at most `maxBytes`). NotFound when it does not exist, IoError when it cannot be
/// read, InvalidArgument when it is larger than the limit.
std::expected<std::string, Error> readFileLimited(const std::filesystem::path& path,
                                                  std::size_t maxBytes = kMaxInputBytes);
/// Reads at most the first `maxBytes` bytes.
std::expected<std::string, Error> readFilePrefix(const std::filesystem::path& path, std::size_t maxBytes);

/// "00:00:05;12" — the timecode of the frame at `t` in a sequence of this rate.
std::string timecodeLabel(Ticks t, Rate rate, bool dropFrame);
/// "30 frames" / "1 frame" / "12.5 frames" for a duration at `rate`.
std::string framesLabel(Ticks duration, Rate rate);

/// "<prefix>: <reason>" errors for importers.
Error parseError(std::string message);
Error unsupported(std::string message);

} // namespace wabcut::interchange::detail
