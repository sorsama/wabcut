#pragma once

// file:// URLs and paths as interchange files write them.
//
// Editors spell the same file many ways: Premiere on Windows writes "file://localhost/C%3a/Media/A001.mov",
// FCP7 on macOS "file://localhost/Volumes/Media/A001.mov", FCPXML and OTIO "file:///Users/me/A001.mov",
// some tools a bare path or a relative one. These helpers turn any of them into a UTF-8 path string with
// '/' separators, without touching the file system.

#include <wabcut/model/Error.h>

#include <expected>
#include <string>
#include <string_view>

namespace wabcut::interchange {

/// Decodes %XX escapes (any case) into bytes. A '%' not followed by two hex digits is an error, as is a
/// decoded NUL byte. The result is not checked for UTF-8 validity (see isValidUtf8).
std::expected<std::string, Error> percentDecode(std::string_view text);

/// True when `text` is well-formed UTF-8 (no overlong forms, surrogates or code points above U+10FFFF).
bool isValidUtf8(std::string_view text);

/// True for "file:" URLs (case-insensitive scheme).
bool isFileUrl(std::string_view text);

/// Converts a media reference to a path:
///  * "file://localhost/C%3a/a%20b.mov", "file:///C:/a b.mov", "file:/C:/a.mov" -> "C:/a b.mov"
///  * "file://localhost/Volumes/x.mov", "file:///Users/x.mov"                -> "/Volumes/x.mov"
///  * "file://server/share/x.mov" (UNC)                                    -> "//server/share/x.mov"
///  * a path without a scheme is returned as is, with '\' turned into '/'.
/// Query strings and fragments are removed. Other schemes (http:, ...) are ParseError; so is an empty
/// path or one that decodes to invalid UTF-8.
std::expected<std::string, Error> mediaReferenceToPath(std::string_view reference);

enum class FileUrlStyle : unsigned char {
    /// "file://localhost/C%3a/Media/A001.mov", "file://localhost/Volumes/Media/A001.mov": FCP7 XML as Premiere Pro
    /// writes it.
    Localhost,
    /// "file:///C:/Media/A001.mov", "file:///Users/me/A001.mov": FCPXML and OpenTimelineIO.
    Triple,
};

/// The file URL of an absolute UTF-8 path ('/' or '\' separators): bytes outside the unreserved set and '/' are
/// percent-encoded (lower-case hex, as Premiere writes them), a drive's ':' too in the Localhost style. A UNC path
/// "//server/share/x" becomes "file://server/share/x". mediaReferenceToPath() reads every result back unchanged. A
/// relative path is encoded without a scheme.
std::string pathToFileUrl(std::string_view path, FileUrlStyle style);

/// True for "C:/..." or "C:\..." (a Windows drive path), "//server/share" or "\\server\share" (UNC).
bool isWindowsAbsolutePath(std::string_view path);

/// True for paths that are absolute on some platform: POSIX "/...", or isWindowsAbsolutePath.
bool isAbsoluteOnAnyPlatform(std::string_view path);

/// Last path component, accepting both '/' and '\' separators ("C:\a\b.mov" -> "b.mov").
std::string_view fileNameOf(std::string_view path);

} // namespace wabcut::interchange
