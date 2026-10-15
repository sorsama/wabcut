#pragma once

// .wabcut project files: UTF-8 JSON with an integer schemaVersion.
//
// All times are exact integer ticks (int64) and rates are integer ratios, so a save/load round trip is
// lossless; parameter values are doubles written with round-trip precision. Unknown keys are ignored,
// but known fields with malformed or out-of-domain values (wrong type, unknown enum name, negative
// size, unknown parameter key, broken invariants) are rejected with ErrorCode::ParseError.
//
// Schema history (each step has a migration in ProjectIO.cpp; older files load and are written back in
// the current schema):
//   1  M2: media path/proxyPath, clip "properties" (position/scale/... objects)
//   2  rev 2: media "locator" (relative path, fingerprint, offline, proxy provenance), sequence
//      "duration"/"workArea", clip "parameters" keyed by stable parameter ids, track "transitions",
//      chroma location and alpha mode in media info
//   3  C.3 finishing: keyframe curves (a parameter is {"value": x} or {"keys": [{"t", "v", "i", "in", "out"}]}),
//      clip "effects" stacks, "reversed", "maintainPitch", freeze frames (speed 0/1), transition "params",
//      track and sequence (master) "effects". Unknown effects and parameter keys round-trip unchanged.
//   4  AI-3: sequence "captionTracks" ({id, name, language, enabled, cues: [{start, duration, text}]}).
//   5  C.4: media "generator" ({kind, text, fontFamily, fontStyle}) for synthetic items without a file (titles).
//   6  C.5: sequence "beatGrid" ({bpmMilli, phase, beatsPerBar, beats: [ticks]}) for beat-snapping.
//   7  AI-4: audio track "role" ("dialogue" | "music" | "effects"; absent = none) and the sequence's master audio
//      effect stack "audioEffects" (absent = empty).
//   8  C.6: transition "alignment" ("center" | "startAtCut" | "endAtCut" | "custom"; the migration derives it from
//      "before"/"after": nothing before the cut = startAtCut, nothing after = endAtCut, sides within a frame of
//      each other = center, otherwise custom), clip "timeRemap" (a curve: {"keys": [...]}, TimeRemap.h).
//   9  C.7: caption track "style" (absent = the look captions had before: white text, boxed, at the bottom), cue
//      "speaker"; project "speakers" ([{id, name, color}]), "transcriptEdits" ([{media, audioStream, labels:
//      [[label, speaker]], speakerSpans: [{start, duration, speaker}], corrections: [{start, duration, original,
//      text}]}]) and "captionPresets" ([{name, style}]), each written only when there are some.
//   10 C.8: clip "source" in place of "media" for a clip of a sequence ({"type": "sequence", "sequence": id,
//      "angles": [i, ...]} for nested and multicam clips; a source "type" this build does not know is kept as it is
//      and saved back unchanged, like unknown effects), sequence "multicam" ({sync, angles: [{name, video, audio:
//      [track ids], offset}]}) on multicam sources, media info "startTimecode" (ticks). Builds before C.8 refuse
//      schema 10 files (VersionMismatch) rather than half-load them.
//   11 C.9: project "reviewNotes" ([{id, sequence, start, duration, mark?: {x, y, width, height}, author, text,
//      createdAt, status: "open" | "resolved", replies: [{author, text, createdAt}]}]), written only when there are some.
//   12 AI-14: mask shape "matte" with its "matte" track (AI object masks). It also marks the fields A.8 and A.9
//      added while the schema stayed 11 (review E5-P2-8): media "recording" (voice-over provenance), track
//      "recordArmed"; clip and media "channels" (channel maps), audio track "format" and "surround", sequence
//      "masterFormat". Builds before it refuse schema 12 files (VersionMismatch) rather than drop them on save.
//   13 C.13: clip "graphic" (GraphicJson.h: layers, responsive time, template instance), project "brandKit" and
//      "templates" (written only when set), caption style "brand" (brand kit references). Builds before C.13 refuse
//      schema 13 files rather than drop the graphics on their next save. Every merge that adds a field bumps the
//      schema (test_project_io's key guard, tests/data/project-file-keys.txt).
//   14 X.1: sequence "startTimecode" (ticks; written only when set). Builds before X.1 refuse schema 14 files rather
//      than drop a sequence's start timecode on their next save.
//   15 C.15: clip "blendMode" (Blend.h; written only when not normal), media generator "params" (Synthetic.h: synthetic
//      items' settings; written only when set) and the generator kinds of synthetic items and adjustment layers. Builds
//      before C.15 refuse schema 15 files rather than drop blend modes or play synthetic items as unknown generators.
//   16 A.10: media info audio stream "title" (the stream's name in the file: MKV track name, MP4 handler name; written
//      only when set). Builds before A.10 refuse schema 16 files rather than drop the names on their next save.
// Files from a newer schema are rejected with ErrorCode::VersionMismatch rather than half-loaded.
//
// Saving is atomic (temporary file in the same directory, flushed to disk, renamed over the project)
// and keeps rotated recovery copies "<file>.bak1" (newest) … "<file>.bak5" of earlier valid saves.
// Loading never writes, renames or deletes anything.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::io {

inline constexpr int kSchemaVersion = 16;
inline constexpr int kOldestSchemaVersion = 1;
inline constexpr std::string_view kFormatName = "wabcut-project";
inline constexpr std::string_view kProjectExtension = ".wabcut";
inline constexpr int kDefaultBackupCount = 5;

/// Deepest JSON nesting (arrays and objects) a project file, history chunk or review package may have: far above any
/// real file (about 15), and low enough that reading and writing what is kept as JSON cannot exhaust the stack.
inline constexpr std::size_t kMaxJsonNesting = 256;

/// Whether the JSON nesting of `text` stays within `maxDepth` (a quick scan before parsing untrusted text; it does not
/// check that the text is JSON).
bool jsonNestingWithin(std::string_view text, std::size_t maxDepth = kMaxJsonNesting);

/// C.15, the preview render cache (PreviewCache.h): the canonical JSON text of everything that decides what `sequence`
/// shows over `range` - its picture settings and master video stack, every video track's switches and stack, the clips
/// and transitions over the range (as saved, without ids, names, label colours and link groups; times relative to
/// range.start), the media they show (file identity, stream info, generator, subclip range, the redactions covering
/// it) and the nested sequences they play (whole, recursively, at most 8 levels). Equal text = equal pixels; captions
/// and audio are not part of it. Empty for an unknown sequence.
std::string renderKeyJson(const ProjectState& state, SequenceId sequence, TimeRange range);

/// Serializes the project (pretty-printed JSON, current schema). Fails only on strings that are not valid UTF-8.
std::expected<std::string, Error> toJson(const ProjectState& state);

/// Parses a project of any supported schema, migrating it to the current one. The result passes
/// validate(…, Validation::Structure).
std::expected<ProjectState, Error> fromJson(std::string_view json);

/// Writes `contents` to `file` atomically: a temporary file in the same directory is written, flushed to
/// disk and renamed over `file`, so readers see either the old or the new contents, never a mix.
std::expected<void, Error> writeFileAtomically(const std::filesystem::path& file, std::string_view contents);

struct SaveOptions {
    /// Recovery copies to keep (0 = none). The previous file is rotated into "<file>.bak1" only when it
    /// is a loadable project, so the copies always hold valid saves.
    int backups = kDefaultBackupCount;
};

/// Saves atomically. The text is checked to load back before anything on disk is touched.
std::expected<void, Error> saveProject(const ProjectState& state, const std::filesystem::path& file,
                                       SaveOptions options = {});
/// Loads a project file. Never modifies, renames or deletes any file, also when it fails.
std::expected<ProjectState, Error> loadProject(const std::filesystem::path& file);

/// "<file>.bak<n>" (n = 1 is the newest copy).
std::filesystem::path backupPath(const std::filesystem::path& projectFile, int n);

/// "<file>.autosave" next to the project file.
std::filesystem::path autosavePath(const std::filesystem::path& projectFile);

/// Atomically writes the project to autosavePath(projectFile) (no rotated copies).
std::expected<void, Error> writeAutosave(const ProjectState& state, const std::filesystem::path& projectFile);

/// A file a crashed or damaged project can be recovered from.
struct RecoveryCandidate {
    enum class Kind { Autosave, Backup };

    std::filesystem::path file;
    Kind kind = Kind::Autosave;
    int backupIndex = 0; ///< 1..n for backups
    std::filesystem::file_time_type modified;
    /// Written after the project file (or the project file is missing): it may hold unsaved work.
    bool newerThanProject = false;
    /// It parses and validates as a project.
    bool loadable = false;
};

/// The autosave and backups of a project, newest first. Offer them when the project fails to load or the
/// autosave is newer than the project (an unclean exit); load the chosen one with loadProject().
std::vector<RecoveryCandidate> recoveryCandidates(const std::filesystem::path& projectFile);

/// Autosave helper for a periodic timer: writes only when the session snapshot changed since the last
/// write (or since markSaved()).
class Autosaver {
public:
    explicit Autosaver(std::filesystem::path projectFile) : m_projectFile(std::move(projectFile)) {}

    /// Writes the autosave if `snapshot` is a different revision than the last one written.
    /// Returns true when a file was written.
    std::expected<bool, Error> save(const std::shared_ptr<const ProjectState>& snapshot);

    /// Records `snapshot` as already on disk (after a manual save), so it is not autosaved again.
    void markSaved(std::shared_ptr<const ProjectState> snapshot) { m_lastSaved = std::move(snapshot); }

    const std::filesystem::path& projectFile() const { return m_projectFile; }

private:
    std::filesystem::path m_projectFile;
    std::shared_ptr<const ProjectState> m_lastSaved;
};

} // namespace wabcut::io
