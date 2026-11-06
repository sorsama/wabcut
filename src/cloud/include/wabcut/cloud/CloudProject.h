#pragma once

// Cloud projects on the user's own storage (0.7, docs/cloud.md). The project stays an ordinary .wabcut file on the computer; a
// cloud project adds a copy on a RemoteStore (a synced folder, WebDAV, S3) and the rules that keep several editors from losing
// each other's work.
//
// Remote layout, below the store's root (one folder per project, named by a random 128-bit id):
//   <id>/head.json                         the one mutable file: which revision is current (compare-and-swap on its ETag)
//   <id>/lock.json                         the editing lock (CloudLock.h)
//   <id>/revisions/<rev8>-<sha16>.wabcut   one immutable revision of the project (never overwritten: its name holds its hash)
//   <id>/revisions/<rev8>-<sha16>.json     its meta: revision, sha256, parent, who, when, schema, app version
// Media is never uploaded.
//
// Revisions are identified by (number, sha256), not by number alone, and every revision names its parent. A sync therefore
// recognises a divergence even when two computers wrote the same number (a synced folder keeps one of two head.json files), and
// merges against the COMMON ANCESTOR found through the parent links. Revisions that head no longer reaches stay listed
// (unmerged) rather than vanishing.
//
// What is compared, merged and hashed is the project without what belongs to one computer (media paths, offline flags, proxies,
// file times): normalizeForSync(). After a merge this computer's own media locators are put back; media that came from another
// computer arrives with a path under the project folder and is offline until it is relinked. A path that points at another
// computer's network share (\\host\share, //host) is never kept (it would make this computer connect to it).

#include <wabcut/cloud/CloudLock.h>
#include <wabcut/cloud/RemoteStore.h>
#include <wabcut/model/Merge.h>
#include <wabcut/model/ProjectState.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace wabcut::cloud {

inline constexpr std::uint64_t kMaxHeadBytes = 64 * 1024;
inline constexpr std::uint64_t kMaxRevisionBytes = std::uint64_t{512} << 20;
inline constexpr std::int64_t kMaxRevisionNumber = 99'999'999;

/// One revision (also the content of head.json for the current one).
struct RevisionMeta {
    std::int64_t revision = 0;
    std::string sha256;       ///< of the revision file's bytes
    std::string parentSha;    ///< "" for the first revision
    std::int64_t parentRevision = 0;
    /// A merge of two lines names the line it took in as a second parent ("" for a plain revision).
    std::string secondParentSha;
    std::int64_t secondParentRevision = 0;
    std::string file;         ///< "revisions/00000007-1a2b3c4d5e6f7a8b.wabcut" (relative to the project's folder)
    std::uint64_t bytes = 0;
    std::string savedBy;      ///< "user on host"
    std::string savedAt;      ///< ISO-8601 UTC
    std::string appVersion;
    int schema = 0;           ///< the project schema of the file
    std::string name;         ///< the project's name in the cloud (what Open Cloud Project lists)
    std::string label;
    bool operator==(const RevisionMeta&) const = default;
};

/// The file name of a revision (without folder): "00000007-1a2b3c4d5e6f7a8b".
std::string revisionStem(std::int64_t revision, std::string_view sha256);
/// The text of a meta / head.json file, and its parse (nullopt for anything that is not one; hostile text is bounded).
std::string revisionJson(const RevisionMeta& meta, bool asHead);
std::optional<RevisionMeta> parseRevisionJson(std::string_view text);

/// The link of a project file to its cloud copy: `<project>.wabcut-cloud` next to it (never holds a secret).
struct CloudLink {
    std::string location;        ///< the id of the configured storage location
    std::string projectId;
    std::string name;
    std::int64_t baseRevision = 0; ///< the revision this computer last synced
    std::string baseSha;           ///< its sha256
    std::string baseNormSha;       ///< sha256 of the normalised project text as it was then (did the project change since?)
    std::string syncedAt;
    bool autoSync = false;         ///< sync after every save (off unless the user turns it on)
    bool operator==(const CloudLink&) const = default;
};

std::filesystem::path cloudLinkPath(const std::filesystem::path& projectFile);
std::expected<CloudLink, Error> readCloudLink(const std::filesystem::path& projectFile);
std::expected<void, Error> writeCloudLink(const std::filesystem::path& projectFile, const CloudLink& link);

// ---- what belongs to one computer ----

/// The project without this computer's media paths, offline flags, proxies and file times (what sync compares and merges).
ProjectState normalizeForSync(const ProjectState& state);
/// Replaces the locators that point at a network share of another computer (UNC, "//host") by a path under `projectFolder`;
/// those items are offline until relinked. Returns how many items changed.
std::size_t quarantineForeignPaths(ProjectState& state, const std::filesystem::path& projectFolder);
/// `merged` (built from normalised states) with this computer's locators back: an item `mine` has keeps `mine`'s locator (offline
/// when the merged item is another file), an item only `theirs` has keeps theirs. Also: the no-cloud lock stays on if either side has it.
ProjectState restoreLocalState(ProjectState merged, const ProjectState& mine, const ProjectState& theirs);

// ---- sync ----

struct SyncRequest {
    ProjectState local;                    ///< the project as it is now
    CloudLink link;
    std::filesystem::path cacheFolder;     ///< base copies are kept here (named by project id and sha256)
    std::filesystem::path projectFolder;   ///< where the project file is (foreign media paths are made relative to it)
    bool mayPush = true;                   ///< false: offline mode, the no-cloud lock, or the editing lock is not ours
    std::string pushBlockedBecause;        ///< what to tell the user when `mayPush` is false and there is something to push
    std::map<std::string, MergeChoice> choices; ///< how conflicts resolve (a first sync passes none)
    bool takeAllTheirs = false;            ///< conflicts not in `choices` take theirs (else mine)
    bool resolveRemaining = false;         ///< apply a merge although conflicts are not all in `choices`
    std::string label;                     ///< names the pushed revision
};

enum class SyncKind : std::uint8_t {
    UpToDate,
    Pushed,         ///< the project was newer: a new revision is on the storage
    FastForwarded,  ///< the storage was newer and the project unchanged: `state` is the cloud's
    Merged,         ///< both changed and merged cleanly; not pushed (`mayPush` was false)
    MergedAndPushed,
    NeedsChoices,   ///< both changed and conflict: nothing changed yet, `conflicts` says what
    PushBlocked,    ///< the project has changes to send but `mayPush` is false
};

struct SyncResult {
    SyncKind kind = SyncKind::UpToDate;
    std::optional<ProjectState> state;      ///< the project to put in the editor (FastForwarded, Merged, MergedAndPushed)
    CloudLink link;                         ///< the new link (write it after the state is applied)
    std::vector<MergeConflict> conflicts;
    std::vector<std::string> notes;         ///< sentences for the user ("3 media items are offline: relink them")
    std::string changes;                    ///< what the cloud changed (ProjectDiff summary)
    RevisionMeta remoteHead;                ///< the cloud's head when the sync started
    std::optional<RevisionMeta> pushed;     ///< the revision this sync wrote
    std::vector<RevisionMeta> newRevisions; ///< the revisions that came with a pull, oldest first (at most 20)
};

struct CreatedProject {
    CloudLink link;
    RevisionMeta revision;
};

struct RemoteProject {
    std::string projectId;
    RevisionMeta head;
};

class CloudProjects {
public:
    CloudProjects(RemoteStore& store, Identity who, UnixClock clock = systemClock(), RandomBytes random = systemRandom(),
                  std::string appVersion = {});

    /// The projects on the storage (those with a readable head.json), newest first.
    StoreResult<std::vector<RemoteProject>> list(std::stop_token stop = {});
    /// Uploads `state` as revision 1 of a new project named `name`. `location` goes into the link.
    StoreResult<CreatedProject> create(const ProjectState& state, std::string name, std::string location,
                                       const std::filesystem::path& cacheFolder, const Progress& progress = {}, std::stop_token stop = {});
    /// The current revision. `etag` (optional) receives head.json's ETag. NotFound when the project has none.
    StoreResult<RevisionMeta> head(std::string_view projectId, std::string* etag = nullptr, std::stop_token stop = {});
    /// The newest revisions, newest first (at most `limit`), from their meta files.
    StoreResult<std::vector<RevisionMeta>> revisions(std::string_view projectId, std::size_t limit = 50, std::stop_token stop = {});
    /// Revisions on the storage that the current head does not reach (written by someone whose push lost, or by a sync client
    /// that kept the other head.json): look at them, then merge by opening each one and syncing.
    StoreResult<std::vector<RevisionMeta>> unmerged(std::string_view projectId, std::size_t limit = 50, std::stop_token stop = {});
    /// Downloads and checks one revision (size and sha256 against its meta, then the project loader's checks). A copy is kept
    /// in `cacheFolder`. The result is exactly what the file holds (no local fix-ups).
    StoreResult<ProjectState> fetch(std::string_view projectId, const RevisionMeta& meta, const std::filesystem::path& cacheFolder,
                                    const Progress& progress = {}, std::stop_token stop = {});
    /// Opens a cloud project on this computer: fetch(head) with foreign network paths quarantined. Returns the link to keep.
    StoreResult<std::pair<ProjectState, CloudLink>> open(std::string_view projectId, std::string location,
                                                         const std::filesystem::path& projectFolder,
                                                         const std::filesystem::path& cacheFolder, std::size_t* quarantined = nullptr,
                                                         const Progress& progress = {}, std::stop_token stop = {});
    /// Brings the project and the cloud copy together (see the header comment and SyncKind).
    StoreResult<SyncResult> sync(const SyncRequest& request, const Progress& progress = {}, std::stop_token stop = {});
    /// Removes a project from the storage (all its revisions). The caller has asked the user.
    StoreResult<std::size_t> remove(std::string_view projectId, std::stop_token stop = {});

    const Identity& identity() const { return m_who; }

private:
    struct Impl;
    StoreResult<SyncResult> syncOnce(const SyncRequest& request, const Progress& progress, std::stop_token stop, bool& raced);
    StoreResult<RevisionMeta> push(const ProjectState& state, const CloudLink& link, const RevisionMeta* parent, std::string_view headEtag,
                                   std::string_view label, const std::filesystem::path& cacheFolder, const Progress& progress,
                                   std::stop_token stop, std::string_view secondParentSha = {}, std::int64_t secondParentRevision = 0);
    StoreResult<std::optional<RevisionMeta>> metaOf(std::string_view projectId, std::string_view sha256, std::int64_t revision,
                                                    std::stop_token stop);
    std::string folderOf(std::string_view projectId) const;

    RemoteStore& m_store;
    Identity m_who;
    UnixClock m_clock;
    RandomBytes m_random;
    std::string m_appVersion;
};

/// Checks that a store keeps its promises about conditional writes before a cloud project is trusted to it: create-if-absent
/// twice (the second must be refused), a write with a stale ETag (must be refused), and a strong ETag from a write. Leaves
/// nothing behind. `problems` empty = the store can hold cloud projects.
struct StoreCheck {
    bool reachable = false;
    bool conditionalWrites = false;
    std::vector<std::string> problems;
};
StoreCheck checkStore(RemoteStore& store, const RandomBytes& random = systemRandom(), std::stop_token stop = {});

} // namespace wabcut::cloud
