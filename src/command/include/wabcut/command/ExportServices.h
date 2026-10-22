#pragma once

// 0.2 (export parity): what the export.* commands drive - the host's export presets, export queue and watch folders.
// The command module stays free of FFmpeg: the host implements this (wabcut_export's ExportCommandServices, which the
// app and wabcut-cli bundle with their other services). Without it, export commands fail with InvalidRequest.
//
// Values are JSON in the formats docs/commands.md shows for the commands: settings as settingsToJson writes them,
// overrides as ExportController.queueExport takes them, jobs as {job, output, preset, state, progress, ...}.

#include <wabcut/command/ExecutionContext.h>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::command {

class ExportServices : public CommandServices {
public:
    /// One export to queue: a preset (built-in or user), overrides on top, and the output file ("" = next to the
    /// project, named after the sequence).
    struct Output {
        std::string preset;
        Json overrides = Json::object();
        std::string output;
    };

    /// [{id, name, description, builtIn, container, videoCodec, settings}]: the built-in presets, then the user's.
    virtual Json presets() const = 0;
    /// What an export of `sequence` with `output` would write: {preset, output, settings, range {start, end}, frames,
    /// duration, width, height, frameRate, extension}. Errors: InvalidParams (an unknown preset, bad overrides, a
    /// range the sequence does not have).
    virtual std::expected<Json, CommandError> describe(const ProjectState& state, SequenceId sequence,
                                                       const Output& output) const = 0;
    /// New job ids for `count` exports (reserved now, used by queue()).
    virtual std::vector<std::string> reserveJobs(std::size_t count) = 0;
    /// Queues the exports (after the request committed): one job per output, under the reserved ids; `hold`: they wait
    /// for "start" (Send to Media Encoder). `state` is the project as the request left it.
    virtual void queue(std::shared_ptr<const ProjectState> state, SequenceId sequence, std::vector<Output> outputs,
                       std::vector<std::string> jobIds, bool hold) = 0;
    /// The queue: [{job, output, preset, state ("held" | "queued" | "running" | "paused" | "done" | "failed" |
    /// "cancelled"), progress (0..1), detail}].
    virtual Json jobs() const = 0;
    /// "start", "pause", "resume", "cancel" or "remove" a job. Errors: NotFound.
    virtual std::expected<void, CommandError> control(std::string_view job, std::string_view action) = 0;

    /// Save Preset: the settings of `from` (a preset + overrides) under `name`. Returns the preset ({id, name, ...}).
    virtual std::expected<Json, CommandError> savePreset(const std::string& name, const std::string& description,
                                                         const Output& from, bool overwrite, bool dryRun) = 0;
    /// Import Preset from a preset file. Returns the preset.
    virtual std::expected<Json, CommandError> importPreset(const std::string& file, bool overwrite, bool dryRun) = 0;
    /// Export Preset to a file (a built-in or user preset).
    virtual std::expected<void, CommandError> exportPreset(const std::string& id, const std::string& file, bool dryRun) = 0;
    /// Delete a user preset.
    virtual std::expected<void, CommandError> deletePreset(const std::string& id, bool dryRun) = 0;

    /// Watch folders: [{id, folder, preset, output, moveSources, queued, done, failed, lastError}].
    virtual Json watchFolders() const = 0;
    virtual std::expected<Json, CommandError> addWatchFolder(const std::string& folder, const std::string& preset,
                                                             const std::string& output, bool moveSources, bool dryRun) = 0;
    virtual std::expected<void, CommandError> removeWatchFolder(std::string_view id, bool dryRun) = 0;
};

} // namespace wabcut::command
