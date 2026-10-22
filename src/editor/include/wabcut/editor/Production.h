#pragma once

// Productions and project locks (0.2, P.3: Premiere's Productions, File > New > Production).
//
//   * A production is a folder on disk holding projects (".wabcut" files) and folders of projects, marked by a
//     "production.wabcut-production" file (format, version, name). Its projects share the folder's media and can
//     bring each other's sequences, bins and media in (edit::importProjectItems through loadProjectForImport): items are
//     copied into the project that imports them, still playing the same files (no media is copied on disk, and media the
//     project already has is reused).
//   * Project locking (Premiere's "Project Locking", on for projects in a production): opening a project writes
//     "<project>.lock" naming the session (user, host, process, since); a project another live session has locked
//     opens read-only. A lock whose process is gone on this host is stale and taken over. Locks are advisory files:
//     they protect editors who share a folder (a network drive, a synced folder), not against other programs.
//   * Several projects open: the app keeps them loaded at once (ProjectLifecycle parks the others with their undo
//     history), and the Production panel also browses any project read only (loadProjectForImport).
//   * Production Settings (Premiere's Production Settings > Scratch Disks): where its projects' preview renders and
//     media cache (waveforms, filmstrips) go, the same for every project in it.
//
// Pure C++, no Qt. Files are untrusted: every read is bounded.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::editor {

inline constexpr std::string_view kProductionFileName = "production.wabcut-production";
inline constexpr std::string_view kProductionFormat = "wabcut-production";
inline constexpr int kProductionVersion = 1;
/// Bounds on what a production lists (a production is a folder anyone can put files in).
inline constexpr std::size_t kMaxProductionEntries = 10000;
inline constexpr int kMaxProductionDepth = 8;

/// Who holds a project.
struct ProjectLock {
    static constexpr std::size_t kMaxFieldBytes = 256;

    std::string user;
    std::string host;
    std::int64_t pid = 0;
    std::string since; ///< ISO-8601 UTC

    /// "user on host".
    std::string owner() const;
    bool operator==(const ProjectLock&) const = default;
};

/// "<project>.lock".
std::filesystem::path projectLockPath(const std::filesystem::path& project);
/// This session as a lock holder (the user, this computer, this process, now).
ProjectLock currentLockHolder();
/// The lock on `project`; nullopt when there is none or the file is not a lock.
std::optional<ProjectLock> readProjectLock(const std::filesystem::path& project);
/// Whether `lock` is another live session's: another computer, or this computer and another running process.
bool heldByOther(const ProjectLock& lock);
/// Locks `project` for this session. When another live session holds it, nothing is written and its lock is returned
/// (the project then opens read-only); otherwise the lock is (re)written and nullopt returned. IoError when the lock
/// cannot be written.
std::expected<std::optional<ProjectLock>, Error> acquireProjectLock(const std::filesystem::path& project);
/// Removes this session's lock on `project` (another session's lock is left alone).
void releaseProjectLock(const std::filesystem::path& project);

/// Premiere's Production Settings > Scratch Disks, shared by the production's projects. Each is a folder (absolute, or
/// relative to the production folder); empty = the default: previews next to each project ("<project> Preview
/// Files"), the media cache in the app's cache folder.
struct ProductionSettings {
    static constexpr std::size_t kMaxPathBytes = 4096;

    std::string previews;
    std::string mediaCache;

    bool operator==(const ProductionSettings&) const = default;
};

/// A project or folder in a production.
struct ProductionEntry {
    std::filesystem::path path;  ///< absolute
    std::string relative;        ///< UTF-8, '/' separators, from the production folder
    std::string name;            ///< the folder's name, or the project's file name without extension
    bool folder = false;
    int depth = 0;               ///< 0 = directly in the production folder
    std::optional<ProjectLock> lock;
    bool lockedByOther = false;
};

class Production {
public:
    /// Makes `folder` (created when missing) a production named `name` (default: the folder's name). Refuses a folder
    /// that already is one (InvalidArgument) or inside one.
    static std::expected<Production, Error> create(const std::filesystem::path& folder, std::string name = {});
    /// The production in `folderOrFile` (its folder or its production file). NotFound when it is not a production.
    static std::expected<Production, Error> open(const std::filesystem::path& folderOrFile);
    /// The production folder a project file lies in (its folder or one up to kMaxProductionDepth levels above), or
    /// nullopt.
    static std::optional<std::filesystem::path> folderOf(const std::filesystem::path& projectFile);

    const std::filesystem::path& folder() const { return m_folder; }
    const std::string& name() const { return m_name; }
    /// Renames the production (its file; the folder keeps its name, as in Premiere).
    std::expected<void, Error> rename(std::string name);
    const ProductionSettings& settings() const { return m_settings; }
    /// Saves new Production Settings in the production file (InvalidArgument for a path that is too long).
    std::expected<void, Error> setSettings(ProductionSettings settings);
    /// A setting as an absolute folder: a relative one under the production folder; empty stays empty.
    std::filesystem::path resolve(const std::string& setting) const;
    /// Where a project's preview renders go: "<previews>/<project> Preview Files" when the production sets previews,
    /// else empty (the default, next to the project).
    std::filesystem::path previewFolderFor(const std::filesystem::path& projectFile) const;

    /// Its folders and projects, depth first, each folder's entries sorted by name (folders first); autosaves, backups
    /// and hidden files are left out.
    std::vector<ProductionEntry> entries() const;
    /// A new folder (relative to the production; "" = its top). Returns its path.
    std::expected<std::filesystem::path, Error> newFolder(std::string_view relative, std::string_view name);
    /// A new project file `name`.wabcut in folder `relative` holding `initial`. Returns its path.
    std::expected<std::filesystem::path, Error> newProject(std::string_view relative, std::string_view name,
                                                           const ProjectState& initial);
    /// Add Project to Production: a copy of `project` in folder `relative`, its media still playing the same files
    /// (paths resolved where the project was, relative paths made for the new place). Returns the copy's path.
    std::expected<std::filesystem::path, Error> addProject(const std::filesystem::path& project,
                                                           std::string_view relative = {});
    /// Whether `path` lies inside the production folder.
    bool contains(const std::filesystem::path& path) const;

private:
    Production(std::filesystem::path folder, std::string name, ProductionSettings settings = {})
        : m_folder(std::move(folder)), m_name(std::move(name)), m_settings(std::move(settings)) {}
    std::expected<std::filesystem::path, Error> folderAt(std::string_view relative) const;

    std::filesystem::path m_folder;
    std::string m_name;
    ProductionSettings m_settings;
};

/// The production `projectFile` lies in, opened (nullopt when it is in none or its file cannot be read).
std::optional<Production> productionOf(const std::filesystem::path& projectFile);

/// Another project read for browsing and import: loaded (any schema), every media file and LUT checked on disk where
/// that project is (editor::withMediaChecked), and relative paths made for `targetProject` (empty: none, untitled).
/// Never writes anything.
std::expected<ProjectState, Error> loadProjectForImport(const std::filesystem::path& source,
                                                        const std::filesystem::path& targetProject);
/// The same for a project already in memory (one open next to the one being edited, with its unsaved changes), whose
/// file is `sourceFile`.
ProjectState preparedForImport(const ProjectState& state, const std::filesystem::path& sourceFile,
                               const std::filesystem::path& targetProject);

} // namespace wabcut::editor
