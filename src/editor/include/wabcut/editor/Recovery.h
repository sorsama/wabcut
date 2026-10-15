#pragma once

// The recovery dashboard (I.4): every version of a project a user might want back, with enough to choose: the
// project file as saved now, its autosaves (rotated) and backups (".bak1".."bakN"), and untitled projects'
// autosaves; each summarised (sequences, clips, duration, last edit) and pictured by a poster frame rendered with the
// export renderer (the same pixels as the monitor). A chosen version opens as an untitled copy
// (ProjectDocument::recover(file, {})) so the project file itself is never overwritten by accident.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace wabcut::editor {

struct ProjectSummary {
    bool loadable = false;
    std::string error; ///< why it does not load
    int sequences = 0;
    int clips = 0;
    int media = 0;
    int captionCues = 0;
    /// The first sequence (the one a project opens on).
    std::string mainSequence;
    Ticks duration{};
    Rate frameRate{};
    int width = 0;
    int height = 0;
    std::int64_t bytes = 0;
    std::filesystem::file_time_type modified{};
};

/// Counts of a project state (no disk access).
ProjectSummary summarizeState(const ProjectState& state);
/// Loads `file` (any project, autosave or backup) and summarises it; `loadable` false with the error otherwise.
ProjectSummary summarizeProjectFile(const std::filesystem::path& file);

struct RecoveryEntry {
    enum class Kind {
        Current,          ///< the project file as saved now (what the others are compared with)
        Autosave,         ///< "<file>.autosave", ".autosave2", ... (index 1 = the newest)
        Backup,           ///< "<file>.bak1".. (index 1 = the newest)
        UntitledAutosave, ///< an untitled project's autosave
    };
    std::filesystem::path file;
    Kind kind = Kind::Current;
    int index = 0;
    /// The project it is a version of; empty for untitled autosaves.
    std::filesystem::path project;
    bool newerThanProject = false;
    ProjectSummary summary;
};

/// The versions of `projectFile` (it first when it exists, then its autosaves and backups, newest first) followed by
/// the untitled autosaves in `untitledDir` (newest first). `autosaveCopies` rotated autosaves are looked for.
std::vector<RecoveryEntry> recoveryEntries(const std::filesystem::path& projectFile, const std::filesystem::path& untitledDir,
                                           int autosaveCopies = 3);

/// One rendered poster: RGBA8, tightly packed, top row first.
struct PosterImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    Ticks time{}; ///< sequence time shown
};

/// The poster frame of `state`'s first sequence: its picture at a third of its content, `width` pixels wide (the
/// sequence's aspect), rendered by the export renderer from the original media (checked on disk relative to
/// `projectFile`; offline media shows nothing). Errors: NotFound (no sequence), DeviceError (no GPU renderer), decode
/// errors.
std::expected<PosterImage, Error> renderPoster(const ProjectState& state, const std::filesystem::path& projectFile,
                                               int width = 320);

} // namespace wabcut::editor
