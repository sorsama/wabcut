#pragma once

// Review notes (C.9): frame-accurate comments on a sequence for client review - a time or range, an optional point or
// rectangle on the frame, an author, open/resolved, and threaded replies - kept in the project
// (ProjectState::reviewNotes). Review packages (command layer) carry them to a reviewer and back; notes can become
// sequence markers.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

enum class ReviewStatus : std::uint8_t { Open, Resolved };

std::string_view toString(ReviewStatus status);
std::optional<ReviewStatus> reviewStatusFromString(std::string_view name);

/// Where on the frame a note points: a rectangle in frame fractions (0..1 from the top left); width = height = 0 is a
/// point.
struct ReviewMark {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const ReviewMark&) const = default;
};

struct ReviewReply {
    std::string author;
    std::string text;
    std::string createdAt; ///< ISO-8601 UTC, or empty

    bool operator==(const ReviewReply&) const = default;
};

struct ReviewNote {
    ReviewNoteId id;
    SequenceId sequence;
    /// Sequence time it is about: a frame (duration 0) or a range.
    Ticks start;
    Ticks duration;
    std::optional<ReviewMark> mark;
    std::string author;
    std::string text;
    std::string createdAt; ///< ISO-8601 UTC, or empty
    ReviewStatus status = ReviewStatus::Open;
    std::vector<ReviewReply> replies; ///< in the order they were written

    Ticks end() const { return start + duration; }
    bool operator==(const ReviewNote&) const = default;
};

/// Bounds (project files and review packages are untrusted).
inline constexpr std::size_t kMaxReviewNotes = 10'000;
inline constexpr std::size_t kMaxReviewReplies = 1'000;
inline constexpr std::size_t kMaxReviewTextBytes = 8 * 1024;
inline constexpr std::size_t kMaxReviewAuthorBytes = 128;

/// What is wrong with a note on its own (sizes, valid UTF-8, a mark inside the frame, a non-negative range), or nullopt.
std::optional<std::string> reviewNoteProblem(const ReviewNote& note);

} // namespace wabcut
