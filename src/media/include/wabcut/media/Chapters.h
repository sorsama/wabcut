#pragma once

// The chapter markers of a recording (P0 round, OBS chapter markers on import): OBS Studio's "Add Chapter Marker" hotkey
// writes them into hybrid MP4 and MKV recordings, and other muxers write them too (Matroska chapters, MP4 Nero / QuickTime
// chapter tracks). Import turns them into clip markers (editor::chapterMarkers); the highlight finder reads them as
// hints (ai::highlights::readChapters). Reads headers only (no stream probing, no decoding). Untrusted: bounded.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::media {

inline constexpr std::size_t kMaxChapters = 10000;        ///< chapters read per file
inline constexpr std::size_t kMaxChapterTitleBytes = 200; ///< longer titles are cut (at a UTF-8 boundary)

struct MediaChapter {
    /// Where the chapter starts, on the demuxer's chapter clock (for recordings, which start at 0, the source time).
    Ticks start;
    /// Where it ends (>= start; == start when the file gives no usable end).
    Ticks end;
    /// UTF-8 on one line (invalid bytes read as U+FFFD, control characters as spaces, trimmed), at most
    /// kMaxChapterTitleBytes; "" when the chapter has no title.
    std::string title;

    bool operator==(const MediaChapter&) const = default;
};

/// A chapter title as MediaChapter::title holds it, from the file's raw bytes (fix round 1: leading spaces and control
/// characters never count toward the bound).
std::string cleanChapterTitle(std::string_view raw);

/// The chapters of `path` (UTF-8) in the file's order, at most kMaxChapters; chapters that start before 0 or have no
/// time base are left out; none when the file has no chapters. Errors as media::probe (NotFound, Unsupported).
std::expected<std::vector<MediaChapter>, Error> readChapters(const std::string& path);

} // namespace wabcut::media
