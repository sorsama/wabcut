#pragma once

// Operations on review notes (ReviewNotes.h, C.9). Each returns a new ProjectState; they validate their inputs.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ReviewNotes.h>

#include <expected>
#include <span>

namespace wabcut::edit {

/// Adds a note (its id is assigned) on an existing sequence, starting inside it. Errors: NotFound for the sequence;
/// InvalidArgument for a note reviewNoteProblem() rejects, a start past the sequence's end, or too many notes.
std::expected<Created<ReviewNoteId>, Error> addReviewNote(const ProjectState& state, ReviewNote note);
/// Adds a reply at the end of the note's thread.
std::expected<ProjectState, Error> replyToReviewNote(const ProjectState& state, ReviewNoteId note, ReviewReply reply);
/// Opens or resolves a note.
std::expected<ProjectState, Error> setReviewNoteStatus(const ProjectState& state, ReviewNoteId note, ReviewStatus status);
/// Replaces a note (same id; review package import). Its sequence must exist.
std::expected<ProjectState, Error> updateReviewNote(const ProjectState& state, ReviewNote note);
std::expected<ProjectState, Error> removeReviewNote(const ProjectState& state, ReviewNoteId note);

/// Sequence markers from review notes: `notes` (all the sequence's notes when empty), each a comment marker at the
/// note's range named after the first line of its text, the comment holding the text, the author and the replies, red
/// while open and green once resolved. With `removeNotes` the notes go in the same step.
EditOutcome reviewNotesToMarkers(const ProjectState& state, SequenceId sequence, std::span<const ReviewNoteId> notes,
                                 bool removeNotes = false);

} // namespace wabcut::edit
