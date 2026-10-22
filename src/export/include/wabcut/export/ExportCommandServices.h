#pragma once

// 0.2: the export.* commands' services (command/ExportServices.h) over this module - presets (built-in and the user's
// PresetStore), export requests, the host's export queue, watch folders. The app wires them to ExportController (its
// queue and job list); wabcut-cli and the MCP server use standalone(), which owns a queue of its own.

#include <wabcut/command/ExportServices.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportPresets.h>
#include <wabcut/export/WatchFolders.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::exporter {

class ExportCommandServices : public command::ExportServices {
public:
    /// What the host provides.
    struct Host {
        /// Queues a built request as job `job` (`hold`: it waits for "start"). May post it to another thread.
        std::function<void(std::string job, ExportRequest request, std::string presetName, bool hold)> enqueue;
        /// The queue as ExportServices::jobs() describes it.
        std::function<Json()> jobs;
        /// "start", "pause", "resume", "cancel", "remove" on a job. Errors: NotFound.
        std::function<std::expected<void, Error>(std::string_view job, std::string_view action)> control;
        /// A new, unique job id ("j1", ...). Thread-safe.
        std::function<std::string()> newJobId;
        /// The default output of a sequence's export (empty function: next to the project file, else in the current
        /// folder, named after the sequence, numbered when taken).
        std::function<std::filesystem::path(const Sequence& sequence, const ExportSettings& settings)> defaultOutput;
        std::filesystem::path projectFile;
        VideoSourceFactory videoSource;           ///< empty = the rendered source
        std::shared_ptr<PresetStore> presets;     ///< the user's presets (null: built-in ones only)
        std::shared_ptr<WatchFolderService> watch; ///< null: no watch folders
        std::shared_ptr<PreviewCache> previews;   ///< Use Previews (null: none)
        std::function<void()> presetsChanged;     ///< after a save, import or delete (optional)
        std::function<void()> watchFoldersChanged;
    };

    explicit ExportCommandServices(Host host);

    /// wabcut-cli and MCP: a queue of their own (one worker), job ids "j1", ..., the user's presets from
    /// `presetFolder` (may be empty: built-in only) and watch folders polled every 2 s.
    static std::shared_ptr<ExportCommandServices> standalone(std::filesystem::path projectFile,
                                                             std::filesystem::path presetFolder,
                                                             VideoSourceFactory videoSource = {});
    /// Standalone hosts: blocks until every queued job has finished (wabcut-cli cmd waits before it exits); returns
    /// their final status as jobs() describes them.
    Json waitForJobs();

    /// The settings of a preset (built-in or the user's) with `overrides` on top, validated. Errors: InvalidArgument.
    std::expected<std::pair<Preset, ExportSettings>, Error> settingsFor(const std::string& preset, const Json& overrides) const;

    // command::ExportServices
    Json presets() const override;
    std::expected<Json, command::CommandError> describe(const ProjectState& state, SequenceId sequence,
                                                        const Output& output) const override;
    std::vector<std::string> reserveJobs(std::size_t count) override;
    void queue(std::shared_ptr<const ProjectState> state, SequenceId sequence, std::vector<Output> outputs,
               std::vector<std::string> jobIds, bool hold) override;
    Json jobs() const override;
    std::expected<void, command::CommandError> control(std::string_view job, std::string_view action) override;
    std::expected<Json, command::CommandError> savePreset(const std::string& name, const std::string& description,
                                                          const Output& from, bool overwrite, bool dryRun) override;
    std::expected<Json, command::CommandError> importPreset(const std::string& file, bool overwrite, bool dryRun) override;
    std::expected<void, command::CommandError> exportPreset(const std::string& id, const std::string& file, bool dryRun) override;
    std::expected<void, command::CommandError> deletePreset(const std::string& id, bool dryRun) override;
    Json watchFolders() const override;
    std::expected<Json, command::CommandError> addWatchFolder(const std::string& folder, const std::string& preset,
                                                              const std::string& output, bool moveSources, bool dryRun) override;
    std::expected<void, command::CommandError> removeWatchFolder(std::string_view id, bool dryRun) override;

    /// A preset as presets() lists it.
    static Json presetJson(const Preset& preset, bool builtIn);

private:
    struct Standalone;
    std::expected<ExportRequest, command::CommandError> requestFor(std::shared_ptr<const ProjectState> state, SequenceId sequence,
                                                                   const Output& output, std::string* presetName) const;
    std::shared_ptr<Standalone> m_standalone; // before m_host: its queue outlives the host's watch-folder reference
    Host m_host;
};

} // namespace wabcut::exporter
