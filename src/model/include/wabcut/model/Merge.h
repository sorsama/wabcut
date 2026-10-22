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

/// 0.2 (P.3): what to bring in from another project (Premiere's Import of a project: the whole project or selected
/// sequences; dragging items from another open project's panel). Ids are the other project's.
struct ProjectImportSelection {
    bool everything = false;             ///< the whole project (every bin, sequence and media item)
    std::vector<BinId> bins;             ///< with everything inside them
    std::vector<SequenceId> sequences;   ///< with everything they play
    std::vector<MediaId> media;
};

struct ProjectImportOptions {
    BinId into;                 ///< where the imported items go (invalid = the root bin; a search bin is refused)
    /// Not empty: a new bin of this name in `into` holds them (Premiere makes one named after the imported project).
    std::string binName;
    /// Media whose file the project already has (same path, same content) are not added again: the copies play the
    /// project's own item. Off: every needed item is copied.
    bool reuseMedia = true;
    /// The other project's file (UTF-8, absolute): copied media items and sequences remember it and their id there
    /// (ImportSource). Empty: they record nothing.
    std::string sourceProject;
};

struct ProjectImportResult {
    ProjectState state;
    BinId bin;                          ///< the bin made for the import (binName), else invalid
    std::vector<BinId> bins;            ///< copies of bins, in the other project's id order
    std::vector<SequenceId> sequences;  ///< copies of sequences (selected ones and what they play), same order
    std::vector<MediaId> media;         ///< media items added
    std::size_t reusedMedia = 0;        ///< needed media the project already had
    std::vector<std::string> notes;     ///< what was adjusted or left out, for the import report
};

/// Brings items of `source` (another project, its media already resolved: absolute paths, offline flags) into
/// `state` in one edit: copies with new ids of the selected bins (sub-bins and contents), sequences and media, and of
/// everything they need: the media and nested / multicam sequences their clips play, the LUT assets their effects use,
/// caption speakers, transcript corrections and redactions of copied media, review notes of copied sequences, and graphic
/// templates, text styles and caption presets this project has no item of that name for. Items whose bin is not copied
/// go into `options.into`. Errors: NotFound (an id `source` does not have, or no such bin `into`), InvalidArgument
/// (nothing selected, a search bin as `into`, an invalid result).
std::expected<ProjectImportResult, Error> importProjectItems(const ProjectState& state, const ProjectState& source,
                                                             const ProjectImportSelection& selection,
                                                             const ProjectImportOptions& options = {});

} // namespace edit

} // namespace wabcut
