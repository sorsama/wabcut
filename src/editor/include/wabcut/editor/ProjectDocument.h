#pragma once

// ProjectDocument: the project file behind an EditSession (File > New / Open / Save / Save As), its dirty
// state, autosave with rotated recovery copies, and offline media + relinking.
//
//   * Saving goes through ProjectIO (atomic write, "<file>.bak1..N" rotation of earlier saves). Save As
//     re-bases every media locator's relative path on the new location. 0.7 (card 029): the ".bakN" copies and the
//     autosave files sit beside the project, in its "Wabcut Auto-Save" folder or in a chosen folder
//     (ProjectDocumentOptions::backupLocation, setBackupPolicy); recovery looks in all of those places.
//   * Dirty = the session's snapshot is not the one last opened/saved (an undo back to it is clean again).
//   * Autosave (call autosave() from a timer): writes "<file>.autosave" only when the snapshot changed since the
//     last write. The new copy is written first (atomically, over the current one); only when that succeeded do the
//     older copies rotate ("<file>.autosave2" .. "<file>.autosave<N>", the replaced one becoming "autosave2"), so a
//     failed or interrupted write leaves every existing copy as it was (Codex wave-3 P2-7). An untitled project
//     autosaves to "<untitledAutosaveDir>/Untitled-<id>.wabcut.autosave"; untitledRecoveryFiles() finds every
//     numbered copy of each untitled session and offers its latest loadable one.
//   * Recovery (Codex wave-3 P1-3, P2-11): a recovered revision is dirty and always autosaves again at the next
//     autosave(); an untitled autosave it came from is deleted only after that autosave has written its replacement.
//     Media of a recovery file opened as an untitled copy resolve relative to the project the file belongs to
//     ("<project>.autosave<n>", "<project>.bak<n>" -> "<project>").
//   * Open checks every media file: the absolute path, else the path relative to the project, and the
//     fingerprint (a different file at the old path is offline, not silently used). Files found by their relative
//     path get their absolute path updated; the rest are marked offline (MediaLocator::offline), which playback
//     and the library show. This happens before the session is reset, so a freshly opened project is clean.
//   * Relink: findRelinkCandidates() searches chosen folders by file name (case-insensitive) and checks the
//     fingerprint; relink() points an item at a new file (one undo step), refusing a file whose content differs
//     unless forced.
//
// Pure C++. Mutating members belong to the session's thread (the GUI thread).

#include <wabcut/model/EditSession.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectDowngrade.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::editor {

struct ProjectDocumentOptions {
    /// Rotated recovery copies of manual saves (ProjectIO's "<file>.bakN").
    int saveBackups = io::kDefaultBackupCount;
    /// Autosave files kept: "<file>.autosave" plus N - 1 rotated older ones.
    int autosaveCopies = 3;
    /// Where untitled projects autosave (empty: they do not autosave).
    std::filesystem::path untitledAutosaveDir;
    /// 0.7 (card 029): where a saved project's ".bakN" copies and ".autosave" files are kept (beside it, in its
    /// "Wabcut Auto-Save" folder, or in a folder of the user's choice). Recovery looks in all of them.
    io::BackupLocation backupLocation;
};

/// Online/offline state of one media item, as checked on disk.
struct MediaStatus {
    MediaId media;
    std::string name;
    /// The locator's absolute path.
    std::string path;
    bool offline = false;
    /// Why it is offline ("not found", "the file at this path is a different file").
    std::string reason;
};

/// A file the project uses besides media (a LUT asset, I.5) and whether it is found.
struct AssetStatus {
    AssetId asset;
    std::string kind;
    std::string name;
    std::string path;
    bool offline = false;
};

struct RelinkCandidate {
    std::filesystem::path file;
    /// Same size and sample hash as the locator's fingerprint (always false when the item has none).
    bool fingerprintMatches = false;
    /// P1 round (#42), findRelinkCandidates with `matchStem`: a file named like the item's without the extension that is
    /// the same recording in another container (an OBS .mkv remuxed to .mp4): within a frame of the item's length, the
    /// same picture size and frame rate, the same audio streams and channels. Its bytes differ: never a fingerprint match.
    bool sameRecording = false;
};

/// P1 round (#42): an offline item and the file of its recording in another container.
struct SameRecording {
    MediaId media;
    std::filesystem::path file;
};

struct RecoveryFile {
    std::filesystem::path file;
    enum class Kind { Autosave, Backup } kind = Kind::Autosave;
    std::filesystem::file_time_type modified;
    /// Newer than the project file (unsaved work may be in it).
    bool newerThanProject = false;
    bool loadable = false;
};

class ProjectDocument {
public:
    explicit ProjectDocument(EditSession& session, ProjectDocumentOptions options = {});
    ~ProjectDocument();
    ProjectDocument(const ProjectDocument&) = delete;
    ProjectDocument& operator=(const ProjectDocument&) = delete;

    EditSession& session() const { return m_session; }

    // ---- File ----
    /// The project file; empty while untitled.
    const std::filesystem::path& file() const { return m_file; }
    bool isUntitled() const { return m_file.empty(); }
    /// "<file stem>" or "Untitled".
    std::string displayName() const;
    /// The session's snapshot differs from the one last opened, saved or reset.
    bool isDirty() const;
    /// Marks the current snapshot as clean (e.g. after the app built a default project).
    void markClean();

    /// 0.2 (P.3): what makes this document one project (its file, the revisions last saved and autosaved, its untitled
    /// autosave name), apart from the session. Several projects open at once: the others keep theirs here while
    /// their state and history are parked out of the session (EditSession::Parked).
    struct Identity {
        std::filesystem::path file;
        ProjectSnapshot clean;
        ProjectSnapshot autosaved;
        std::string untitledId;
        std::filesystem::path retire;
    };
    /// Makes this document about `other` and returns what it was about. The session is not touched: exchange its
    /// state too (EditSession::exchange). An empty Identity is an untitled project with a new autosave name.
    Identity exchangeIdentity(Identity other);
    /// Whether a parked project's state differs from what it last opened, saved or reset.
    static bool isDirty(const Identity& identity, const ProjectSnapshot& state);
    /// Deletes a parked project's autosave files (as removeAutosaves() does for this one).
    void removeAutosavesOf(const Identity& identity) const;

    /// Replaces the session's project with `initial` (untitled, clean).
    void newProject(ProjectState initial);
    /// Loads `file` into the session (clean), checking every media file (see the file comment). Nothing changes
    /// when loading fails.
    std::expected<void, Error> open(const std::filesystem::path& file);
    /// Saves to file(); InvalidArgument while untitled.
    std::expected<void, Error> save();
    /// Saves to `file` and makes it the project file. Relative media paths are recomputed for the new location.
    std::expected<void, Error> saveAs(const std::filesystem::path& file);
    /// File > Save a Copy: writes the project as it is now to `file` (relative media paths recomputed for that
    /// location, the save filter applied, like saveAs) and changes nothing here: the project file, the dirty state and
    /// the autosave stay as they were.
    std::expected<void, Error> saveCopy(const std::filesystem::path& file) const;
    /// 0.7 (card 059), File > Save for an Older Version: like saveCopy, but written the way Wabcut `release` ("0.6", "0.5")
    /// reads it (io::saveProjectForRelease); returns what that release cannot hold. Nothing here changes.
    std::expected<io::DowngradeResult, Error> saveForRelease(const std::filesystem::path& file, std::string_view release) const;
    /// What saving for `release` would leave out (no file is written): the same list saveForRelease returns.
    std::expected<std::vector<io::DowngradeLoss>, Error> olderReleaseLosses(std::string_view release) const;

    // ---- Autosave and recovery ----
    /// Writes the autosave when the snapshot changed since the last write (or save). Returns true when a file was
    /// written; false when nothing changed or an untitled project has no autosave folder.
    std::expected<bool, Error> autosave();

    /// autosave() in three parts, so its file work can run off the document's thread (P.1: a 10,000-clip autosave
    /// held the GUI thread ~0.4 s every minute). prepareAutosave() (document thread) takes what is to be written: the
    /// snapshot (immutable; nothing can change it while the job runs), rebased and filtered. AutosaveJob::write() (any
    /// thread) writes the same files as autosave() in the same order: the new copy first, then the older ones rotate,
    /// so a failed write leaves every copy as it was. finishAutosave() (document thread) records a written job.
    /// autosave() is the three in a row.
    struct AutosaveJob {
        ProjectSnapshot snapshot;              ///< the revision written
        ProjectState state;                    ///< what is written (rebased, filtered)
        std::filesystem::path target;          ///< "<project>.autosave" (0.7: in the backup folder when there is one)
        int copies = 1;                        ///< ProjectDocumentOptions::autosaveCopies
        std::filesystem::path retire;          ///< an untitled session recovered from: deleted once written
        bool createFolder = false;             ///< the target's folder may not exist yet
        std::string projectName;               ///< the project file's name for a failure message ("" untitled)
        std::expected<void, Error> write() const;
    };
    /// Nothing to write (unchanged since the last autosave or save, or nowhere to write): nullopt.
    std::optional<AutosaveJob> prepareAutosave() const;
    /// After `job.write()` succeeded: the document knows that revision is autosaved (and the retired file is gone).
    void finishAutosave(const AutosaveJob& job);
    /// Where autosave() writes now (empty when it would not write).
    std::filesystem::path autosaveFile() const;
    /// The autosaves (rotated ones included) and backups of `projectFile`, newest first.
    std::vector<RecoveryFile> recoveryFiles(const std::filesystem::path& projectFile) const;
    /// What an unclean exit left of `projectFile`: its newest autosave (of `autosaveCopies` rotated ones) written after
    /// it that loads, or nullopt. The same file as the first loadable autosave newer than the project in
    /// recoveryFiles(), but only autosaves newer than the project are parsed, newest first, until one loads (P.1:
    /// recoveryFiles() parses every autosave and backup; after a clean exit nothing needs parsing). Any thread.
    /// 0.7 (card 029): looks in every folder `location` may have put the copies in (beside the project, its Auto-Save
    /// folder, the chosen root's), and for at least io::kMaxAutosaveCopies copies in each.
    static std::optional<RecoveryFile> newestUnsavedAutosave(const std::filesystem::path& projectFile, int autosaveCopies,
                                                             const io::BackupLocation& location = {});
    int autosaveCopies() const { return std::max(1, m_options.autosaveCopies); }
    /// 0.7 (card 029): changes the copies kept and where they go. Files already written stay where they are (recovery
    /// still finds them); the next save and autosave use the new settings. `saveBackups` 0-10, `autosaveCopies` 1-10.
    void setBackupPolicy(int saveBackups, int autosaveCopies, const io::BackupLocation& location);
    const io::BackupLocation& backupLocation() const { return m_options.backupLocation; }
    int saveBackups() const { return m_options.saveBackups; }
    /// The folder `file`'s copies go in now (empty: beside it).
    std::filesystem::path backupDirectory(const std::filesystem::path& file) const {
        return io::backupDirectory(file, m_options.backupLocation);
    }
    /// Opens a recovery file as the project `projectFile` (the file to save back to; empty = untitled), dirty. Media
    /// resolve relative to `projectFile`, or (untitled) to the project the recovery file belongs to. The next
    /// autosave() writes the revision whatever was autosaved before; when it is an untitled session's autosave
    /// ("Untitled-<id>.wabcut.autosave<n>"), that file (and that session's older copies) is deleted once the new
    /// session's autosave has been written, never before.
    /// 0.7 (card 029): `mediaBase`, for an untitled copy, is the project file the recovery file is a version of, when
    /// the app knows it (a copy in a chosen backup folder does not say where its project is).
    std::expected<void, Error> recover(const std::filesystem::path& recoveryFile, const std::filesystem::path& projectFile,
                                       const std::filesystem::path& mediaBase = {});
    /// Deletes this document's autosave files (rotated copies included). A clean exit calls it: what is on disk
    /// as the project is then the truth, and the next start has nothing to recover.
    void removeAutosaves();
    /// Edits every state written by save, save as and autosave, after the session's own state is taken (the
    /// proxy registry writes MediaLocator::proxy here: proxies are not part of the undo history). Empty = none.
    void setSaveFilter(std::function<void(ProjectState&)> filter) { m_saveFilter = std::move(filter); }
    /// Autosaves an unclean exit left behind: untitled ones in `untitledAutosaveDir`, newest first. One per untitled
    /// session: the newest of its copies ("...autosave", "...autosave2", ...) that loads.
    static std::vector<RecoveryFile> untitledRecoveryFiles(const std::filesystem::path& untitledAutosaveDir);
    /// The project file a recovery file belongs to: "<project>.autosave<n>" and "<project>.bak<n>" -> "<project>";
    /// a project file is its own. Empty for anything else (an untitled session's autosave included).
    static std::filesystem::path projectOfRecoveryFile(const std::filesystem::path& recoveryFile);

    // ---- Offline media and relinking ----
    /// Every media item's state, from the current snapshot's locators (cheap: no disk access).
    std::vector<MediaStatus> mediaStatus() const;
    std::vector<MediaId> offlineMedia() const;
    /// Re-checks every media file on disk; items whose state changed are updated in one undo step
    /// ("Refresh Media"). Returns how many changed.
    std::expected<int, Error> refreshMediaStatus();
    /// Files named like `media`'s file under `folders` (recursive, case-insensitive), fingerprint matches first. P1 round
    /// (#42): with `matchStem`, files whose name matches without the extension too (probed: sameRecording), after them.
    std::vector<RelinkCandidate> findRelinkCandidates(MediaId media, const std::vector<std::filesystem::path>& folders,
                                                      bool matchStem = false) const;
    /// P1 round (#42): whether `file` is `media`'s recording in another container (RelinkCandidate::sameRecording).
    bool isSameRecording(MediaId media, const std::filesystem::path& file) const;
    /// P1 round (#42), Locate... that relinks the rest: points `media` at `file` like relink() (a forced different file
    /// must still have every stream a clip plays), then, in the same "Relink Media" undo step, every other offline item
    /// whose file sits at the same place under the moved folder (the old root swapped for the new one, up to three
    /// folders up) or under the located file's folder, when its fingerprint matches. Returns how many OTHER items.
    std::expected<int, Error> relinkAfterLocate(MediaId media, const std::filesystem::path& file, bool force = false);
    /// P1 round (#42): offline items with a file under `folders` named like theirs with another extension that is the
    /// same recording (isSameRecording), one file per item.
    std::vector<SameRecording> findSameRecordings(const std::vector<std::filesystem::path>& folders) const;
    /// Relinks `offers` in one "Relink Media" undo step: each item (and its subclips on the same file) gets the new
    /// file and its media info read again (stream indexes included). Refused when a clip plays a stream a new file
    /// lacks. Returns how many items were relinked.
    std::expected<int, Error> relinkSameRecordings(const std::vector<SameRecording>& offers);
    /// Points `media` at `file` (probed again; one undo step "Relink Media"). Refuses (InvalidArgument) a file
    /// whose fingerprint differs from the item's unless `force`.
    std::expected<void, Error> relink(MediaId media, const std::filesystem::path& file, bool force = false);
    /// Relinks every offline item whose file name has a fingerprint-matching candidate under `folders` (one
    /// undo step), offline LUT assets too. Returns the relinked media items.
    std::expected<std::vector<MediaId>, Error> relinkAll(const std::vector<std::filesystem::path>& folders);

    // ---- LUT assets (I.5): located, checked and relinked like media ----
    std::vector<AssetStatus> assetStatus() const;
    std::vector<RelinkCandidate> findAssetRelinkCandidates(AssetId asset, const std::vector<std::filesystem::path>& folders) const;
    /// Points `asset` at `file` (one undo step "Relink LUT", edit::relinkAsset). Refuses a different file unless `force`.
    std::expected<void, Error> relinkAsset(AssetId asset, const std::filesystem::path& file, bool force = false);

private:
    void rememberClean();
    /// Deletes the autosave files (rotated ones included) of the project `file` (every folder they may be in) or of the
    /// untitled session `untitledId`.
    void removeAutosaveFiles(const std::filesystem::path& file, const std::string& untitledId) const;
    std::filesystem::path autosaveFileFor(const std::filesystem::path& file, const std::string& untitledId) const;

    EditSession& m_session;
    ProjectDocumentOptions m_options;
    std::filesystem::path m_file;
    ProjectSnapshot m_clean;     // the snapshot last opened / saved / reset
    ProjectSnapshot m_autosaved; // the snapshot last autosaved (or saved)
    std::string m_untitledId;    // stable name for an untitled project's autosave
    std::function<void(ProjectState&)> m_saveFilter;
    std::filesystem::path m_retire; // an untitled autosave recovered from: deleted after the next successful autosave
};

/// Checks `item`'s file on disk: found at the absolute or the project-relative path, with a matching
/// fingerprint when one is known. Returns the item with its locator updated (absolute path of the file found,
/// offline flag), and the reason when offline.
std::pair<MediaItem, std::string> checkMediaOnDisk(const MediaItem& item, const std::filesystem::path& projectFile);

/// An asset's file checked on disk like media: its absolute path, else its project-relative path, with the
/// fingerprint compared; offline when neither holds it.
std::pair<ProjectAsset, std::string> checkAssetOnDisk(const ProjectAsset& asset, const std::filesystem::path& projectFile);

/// `state` with every media item checked on disk (checkMediaOnDisk), and every asset (checkAssetOnDisk); unchanged
/// items stay shared.
ProjectState withMediaChecked(const ProjectState& state, const std::filesystem::path& projectFile,
                              int* changedCount = nullptr);

} // namespace wabcut::editor
