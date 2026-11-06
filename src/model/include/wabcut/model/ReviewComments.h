#pragma once

// Comments that come back from a review link (0.7, docs/cloud.md): the file a reviewer exports from the review page, and its
// import into the project's review notes (ReviewNotes.h). The reviewer is a stranger and the file is untrusted input: it is
// bounded, validated and cleaned here, and it names no sequence and no time of the project - those come from the record of the
// review link this computer made (ReviewLinkTarget), so a file can only ever put notes on the sequence the link was made from.
//
//   {"format": "wabcut-review-comments", "version": 1, "link": "<public link id>", "reviewer": "Maria",
//    "comments": [{"frame": 120, "text": "The logo is too small", "mark": {"x": 0.7, "y": 0.05, "width": 0.2, "height": 0.1},
//                  "createdAt": "2026-11-05T09:30:00Z"}]}
//
// `frame` counts frames from the first frame of the review copy. The note lands at the copy's start in the sequence plus that
// many frames of the sequence's rate.

#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/ReviewNotes.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

inline constexpr std::string_view kReviewCommentsFormat = "wabcut-review-comments";
inline constexpr int kReviewCommentsVersion = 1;
inline constexpr std::size_t kMaxReviewCommentsBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaxReviewComments = 5000;
/// The suffix of the author of an imported note: "Maria (review link)".
inline constexpr std::string_view kReviewLinkAuthorSuffix = " (review link)";

struct ReviewComment {
    std::int64_t frame = 0;
    std::string text;
    std::optional<ReviewMark> mark;
    std::string createdAt; ///< ISO-8601 UTC or empty
};

struct ReviewCommentsFile {
    std::string link;     ///< the public link id
    std::string reviewer; ///< cleaned, at most 100 bytes; "Reviewer" when empty
    std::vector<ReviewComment> comments;
    std::vector<std::string> skipped; ///< comments left out and why ("comment 3: the text is empty")
};

/// Reads a comments file. Errors: ParseError (not JSON, too large or too deep, wrong format), VersionMismatch (a newer version).
/// Single comments that are wrong are left out and listed in `skipped`. Text is cleaned (control characters and bidirectional
/// overrides removed, valid UTF-8 only).
std::expected<ReviewCommentsFile, Error> parseReviewComments(std::string_view text);

/// What the project's record of a review link says about the copy that was made.
struct ReviewLinkTarget {
    std::string link;      ///< the public link id the file must carry
    SequenceId sequence;
    Ticks start;           ///< where the copy starts in the sequence
    Ticks duration;        ///< how long the copy is (comments past it are left out)
};

struct ReviewCommentsImported {
    ProjectState state;
    std::size_t added = 0;
    std::size_t unchanged = 0;    ///< already there (the same reviewer, time and text)
    std::size_t markers = 0;      ///< sequence markers made from the new notes
    std::vector<std::string> conflicts; ///< comments that could not be placed
    std::vector<ReviewNoteId> notes;    ///< the notes added
};

namespace edit {

/// Adds the comments of `file` as review notes of `target.sequence`, one undo step (the state returned). Errors: InvalidArgument
/// (the file belongs to another link, or the sequence is gone). `now` stamps comments that carry no time of their own.
std::expected<ReviewCommentsImported, Error> importReviewComments(const ProjectState& state, const ReviewCommentsFile& file,
                                                                 const ReviewLinkTarget& target, bool alsoMarkers, std::string_view now);

} // namespace edit

} // namespace wabcut
