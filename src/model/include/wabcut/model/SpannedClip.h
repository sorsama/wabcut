#pragma once

// Spanned clips (0.2, Premiere's Media Browser): one recording a camera wrote as several files - GoPro chapters
// (GX010042.MP4, GX020042.MP4, ...), AVCHD stream files joined by the card's playlist, Canon XF / XDCAM clip parts,
// files split at the FAT32 limit - shown and played as ONE media item.
//
// A spanned item is an ordinary media item whose locator path names the whole run:
//
//     <directory>/<first file>?span=<first file>:<microseconds>|<second file>:<microseconds>|...
//
// Every part is a file in the first file's directory, with the duration it plays (FFmpeg's AV_TIME_BASE units; the
// concat demuxer seeks by them). '?', '|' and ':' cannot occur in a Windows file name, which keeps the path apart from
// any real file. media::detail::openInput plays such a path with FFmpeg's concat demuxer, so probing, decoding,
// waveforms, thumbnails, playback and export need nothing else. The first part stands for the clip in file checks:
// existence, fingerprint and relinking (ImageSequence.h: mediaFileToCheck). Paths read from a project are untrusted:
// parseSpannedClipPath bounds every field and returns nullopt for anything else.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

struct SpannedPart {
    std::string name;              ///< UTF-8 file name in the clip's directory
    std::int64_t microseconds = 0; ///< how long the part plays (> 0)

    bool operator==(const SpannedPart&) const = default;
};

struct SpannedClipSpec {
    std::filesystem::path directory;
    std::vector<SpannedPart> parts; ///< in playing order; at least two

    std::filesystem::path partPath(std::size_t i) const { return directory / partFile(i); }
    std::filesystem::path firstPath() const { return partPath(0); }
    std::int64_t totalMicroseconds() const;
    bool operator==(const SpannedClipSpec&) const = default;

private:
    std::filesystem::path partFile(std::size_t i) const;
};

/// Bounds (untrusted input): parts of one clip, a part's name, a part's duration (24 hours), the whole path.
inline constexpr std::size_t kMaxSpannedParts = 999;
inline constexpr std::size_t kMaxSpannedNameBytes = 255;
inline constexpr std::int64_t kMaxSpannedPartMicroseconds = 24LL * 3600 * 1'000'000;
inline constexpr std::size_t kMaxSpannedPathBytes = 64 * 1024;

/// The media path of a spanned clip (see the file comment). Empty when the spec is out of bounds (fewer than two parts,
/// a name with a separator or one of ?|:*, a duration out of range).
std::string spannedClipPath(const SpannedClipSpec& spec);
/// The spanned clip a media path names, or nullopt for any other path.
std::optional<SpannedClipSpec> parseSpannedClipPath(std::string_view utf8);

} // namespace wabcut
