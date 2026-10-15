#pragma once

// Review packages (C.9): a project's review notes for a client or reviewer, and back.
//
// A package is a folder:
//   notes.json   {"format": "wabcut-review", "version": 1, "title", "createdAt",
//                 "sequences": [{id, name, frameRate: {num, den}, dropFrame}],
//                 "notes": [{id, sequence, start, duration (ticks), timecode, end (timecodes in the sequence's rate),
//                            mark: {x, y, width, height} | null, author, text, createdAt, status, replies: [...],
//                            frame: "frames/note-<id>.<ext>" | null}]}
//   index.html   one self-contained page (styles and thumbnails inline) a reviewer opens in any browser
//   frames/      the thumbnails as files, for other tools
// Thumbnails come from the host's renderer through ReviewFrameSource (PlaceholderFrames draws an SVG card instead, for
// tests and hosts without one). Importing reads notes.json back - an untrusted file: bounded and validated like a
// project - and matches each note to the project by its id (else by sequence, time and text): replies and status from
// the package update a matching note, unknown notes are added, and what cannot be placed or disagrees with the project
// is reported as a conflict and left out.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::command {

struct ReviewFrame {
    std::string mime;  ///< "image/jpeg", "image/png", "image/svg+xml"
    std::string bytes; ///< the encoded image
};

/// The host's thumbnails (a render of the sequence at a time, at most `width` pixels wide).
class ReviewFrameSource {
public:
    virtual ~ReviewFrameSource() = default;
    virtual std::expected<ReviewFrame, Error> frame(const ProjectState& state, SequenceId sequence, Ticks time,
                                                    int width) const = 0;
};

/// A deterministic ReviewFrameSource: an SVG card with the sequence's name and the timecode, at the sequence's aspect.
class PlaceholderFrames final : public ReviewFrameSource {
public:
    std::expected<ReviewFrame, Error> frame(const ProjectState& state, SequenceId sequence, Ticks time,
                                            int width) const override;
};

/// Bound on notes.json read back (packages are untrusted).
inline constexpr std::uintmax_t kMaxReviewPackageBytes = 16u * 1024u * 1024u;

struct ReviewPackageWritten {
    std::size_t notes = 0;
    std::size_t frames = 0;
    std::vector<std::string> warnings; ///< thumbnails the frame source could not make
};

/// Writes a package of `notes` into `folder` (created; files there are replaced). `frames` may be null (no thumbnails).
std::expected<ReviewPackageWritten, Error> writeReviewPackage(const ProjectState& state, const std::filesystem::path& folder,
                                                              std::span<const ReviewNote> notes,
                                                              const ReviewFrameSource* frames, const std::string& title,
                                                              const std::string& createdAt);

/// The self-contained HTML page of a package (thumbnails by note id, inline).
std::string reviewPackageHtml(const ProjectState& state, std::span<const ReviewNote> notes,
                              const std::vector<std::pair<ReviewNoteId, ReviewFrame>>& frames, const std::string& title);

struct ReviewImported {
    ProjectState state;
    std::size_t added = 0;
    std::size_t updated = 0;
    std::size_t unchanged = 0;
    std::vector<std::string> conflicts;
};

/// Reads a package's notes back into `state` (see above).
std::expected<ReviewImported, Error> importReviewPackage(const ProjectState& state, const std::filesystem::path& folder);
/// The same from the text of a package's notes.json (at most kMaxReviewPackageBytes).
std::expected<ReviewImported, Error> importReviewPackageData(const ProjectState& state, std::string_view notesJson);

} // namespace wabcut::command
