#pragma once

// Three-way project merge (C.9, the groundwork for collaboration): two edited copies of a project ("mine", "theirs")
// against the revision both started from ("base"). No networking: the copies are ProjectStates.
//
// How it merges. Ids are stable, so objects match by id. First, the ids "theirs" made since the base are renumbered
// above every id "mine" uses (both sides hand out ids from the same counter, so their new clips may share numbers).
// Then each object is merged three ways: what only one side changed is taken from it (an addition, a removal, a
// change), what both changed the same way is taken once, and what both changed differently is a conflict. The
// granularity: project items (bins, media, LUT assets, redactions, speakers, transcript edits, caption presets, review
// notes) as wholes; a sequence's settings, markers and each caption track as wholes; each track's settings, each clip
// and each transition as wholes. So edits of different sequences, different tracks or different clips merge cleanly.
// Clips both sides placed over the same time on a track are an "overlap" conflict. A clip that moved (to another
// track, or into another sequence: Nest keeps clip ids) is matched across the whole project and placed once: moved on
// one side and changed, moved or removed on the other, it is one conflict "clip:<id>" (E5-P2-5). Nested clips both
// sides added that make two sequences play each other are a conflict "nesting:<sequence id>".
//
// Resolution. Every conflict has a stable id ("clip:812", "sequence:4/settings", "overlap:9:812:930", ...) and is
// resolved by `choices` (mine or theirs); unresolved ones take mine. The merged project is valid: link groups left
// with one clip dissolve, transitions whose clips no longer meet go, and media or sequences a merged clip needs are
// kept. Should the choices still give an invalid project, MergeResult::invalid says why and the conflicts are returned
// all the same. The result is deterministic for the same inputs and choices.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wabcut {

enum class MergeChoice : std::uint8_t { Mine, Theirs };

struct MergeConflict {
    std::string id;      ///< stable: the same inputs give the same ids
    std::string what;    ///< for people
    SequenceId sequence; ///< the sequence it is in, when it is in one
    MergeChoice resolved = MergeChoice::Mine; ///< how the merged project resolved it

    bool operator==(const MergeConflict&) const = default;
};

struct MergeResult {
    ProjectState merged;
    std::vector<MergeConflict> conflicts; ///< sorted by id
    std::size_t fromTheirs = 0;           ///< objects taken from theirs (added, changed or removed there)
    std::vector<std::string> adjustments; ///< what the merge repaired (dissolved links, dropped transitions, kept media)
    /// Set when these choices cannot give a valid project (say why; merge by hand or choose otherwise). `merged` is then
    /// mine, unchanged, and `conflicts` still lists every conflict the merge met, so the caller can stage them.
    std::optional<std::string> invalid;
};

/// Merges `theirs` into `mine`, both edited from `base`. Fails only when an input is not a valid project; a merge
/// that cannot give a valid project with these choices returns its conflicts with MergeResult::invalid set.
std::expected<MergeResult, Error> mergeProjects(const ProjectState& base, const ProjectState& mine,
                                                const ProjectState& theirs,
                                                const std::map<std::string, MergeChoice>& choices = {});

/// `state` with every id from `threshold` up renumbered from `start` (in id order), every reference following: the
/// first step of a merge. Exposed for tests.
ProjectState renumberIdsFrom(const ProjectState& state, std::uint64_t threshold, std::uint64_t start);

namespace edit {

/// "Restore as a new sequence" (version history): a copy of sequence `sequence` of `source` (another revision of the
/// project) added to `state` as `name`, with new ids for everything in it. The media it plays is the project's own when
/// the project still has its id (renamed or relinked since: the id is the source's identity); an item the project no
/// longer has comes back under its own id, so the project's source-anchored redactions of it apply again (review wave 5
/// E5-P1-3). Only an id this project never had is renumbered, and then every redaction of the old id comes along to
/// the new one (fail closed). Speakers and bins the project no longer has are dropped from the copy. Fails when the copy
/// plays a nested sequence the project no longer has.
std::expected<Created<SequenceId>, Error> copySequenceFrom(const ProjectState& state, const ProjectState& source,
                                                           SequenceId sequence, std::string name);

} // namespace edit

} // namespace wabcut
