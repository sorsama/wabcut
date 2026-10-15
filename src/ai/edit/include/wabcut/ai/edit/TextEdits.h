#pragma once

// Text edits -> timeline edits. Deleting, keeping or moving words of a SequenceTranscript is planned as ripple
// deletes (and, for a move, an insert-move) on the tracks the words are heard on and their linked partners, then
// turned into a CommandBatch of the catalog's own commands, so it is validated, previewable (dryRun) and ONE undo
// step. Premiere-faithful details:
//   * cuts fall on frame boundaries just outside the deleted words (padding, default 40 ms each side) and never
//     reach into the neighbouring kept words;
//   * deletions that touch, overlap or have no kept word between them merge into one cut;
//   * linked A/V clips are cut together; locked tracks are never cut (a word heard only on locked tracks cannot
//     be deleted); sync-locked tracks follow every ripple, as with any extract.

#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/command/Command.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace wabcut::ai::edit {

/// Words first..last (inclusive), as indices into SequenceTranscript::words().
struct WordRange {
    std::size_t first = 0;
    std::size_t last = 0;
};

struct CutOptions {
    Ticks padBefore = Ticks{kTicksPerSecond / 25};  ///< 40 ms of the deleted side kept off the previous word
    Ticks padAfter = Ticks{kTicksPerSecond / 25};   ///< 40 ms
    /// For silences: how much of the pause stays on each side of a cut (room tone between the remaining words).
    Ticks roomTone = Ticks{kTicksPerSecond * 15 / 100}; ///< 150 ms
};

/// One ripple delete.
struct PlannedCut {
    TimeRange range;              ///< frame-aligned, on the timeline
    std::vector<TrackId> tracks;  ///< unlocked tracks the words are heard on, with their linked partners
};

/// Cuts in timeline order, never overlapping.
struct EditPlan {
    SequenceId sequence;
    std::vector<PlannedCut> cuts;
    std::size_t words = 0; ///< words removed
    Ticks removed() const;
};

/// What to delete: word ranges and the pauses after words (the gap between word i and i + 1).
struct DeleteRequest {
    std::vector<WordRange> words;
    std::vector<std::size_t> gapsAfter;
    CutOptions options;
};

/// Plans the deletion of words and pauses. Errors: InvalidArgument (bad indices, words only on locked tracks).
std::expected<EditPlan, Error> planDelete(const SequenceTranscript& transcript, const ProjectState& state,
                                          const DeleteRequest& request);

/// Plans a rough cut: keeps the given word ranges (with padding outward) and deletes everything else on the
/// transcript's tracks between the start of the first and the end of the last clip that has words.
std::expected<EditPlan, Error> planKeep(const SequenceTranscript& transcript, const ProjectState& state,
                                        std::span<const WordRange> keep, const CutOptions& options = {});

/// Moving words to another place: the range is cut out with a ripple and inserted at the word boundary before
/// word `before` (== size(): after the last word). The source range and the destination are in timeline time
/// of the state the plan was made on.
struct MovePlan {
    SequenceId sequence;
    TimeRange source;
    Ticks destination;
    std::vector<TrackId> tracks;
};
std::expected<MovePlan, Error> planMove(const SequenceTranscript& transcript, const ProjectState& state, WordRange words,
                                        std::size_t before, const CutOptions& options = {});

/// The plan as catalog commands: one sequence.extract per cut, last cut first. One batch = one undo step.
command::CommandBatch toCommandBatch(const EditPlan& plan, std::string label = "Delete Words");
/// sequence.addEdit at both ends of the range, clip.move (insert mode) of the pieces in it, sequence.extract of
/// the place they left. Needs `state` to name the pieces.
command::CommandBatch toCommandBatch(const MovePlan& plan, const ProjectState& state, std::string label = "Move Words");

/// Runs a batch's commands one after the other on `state` inside a single command (like the engine does for a
/// batch, without committing) and returns the resulting state. Throws command::CommandFailure on the first error.
ProjectState runInline(const command::CommandRegistry& registry, const ProjectState& state,
                       const command::CommandBatch& batch, command::ExecutionControl* control = nullptr,
                       command::CommandServices* services = nullptr);

} // namespace wabcut::ai::edit
