#include <wabcut/export/ExportCommandServices.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Timecode.h>

#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <system_error>

namespace wabcut::exporter {

namespace {

command::CommandError commandError(const Error& e) {
    command::ErrorCode code = command::ErrorCode::InvalidParams;
    switch (e.code) {
    case ErrorCode::NotFound:
        code = command::ErrorCode::NotFound;
        break;
    case ErrorCode::IoError:
    case ErrorCode::Unsupported:
        code = command::ErrorCode::EditRejected;
        break;
    case ErrorCode::Cancelled:
        code = command::ErrorCode::Cancelled;
        break;
    default:
        break;
    }
    return command::CommandError{code, e.message, nullptr};
}

std::string jobState(const ExportJobStatus& s) {
    if (s.held && s.progress.phase == ExportPhase::Queued) {
        return "held";
    }
    switch (s.progress.phase) {
    case ExportPhase::Queued:
        return "queued";
    case ExportPhase::Preparing:
    case ExportPhase::Rendering:
    case ExportPhase::Finalizing:
        return "running";
    case ExportPhase::Paused:
        return "paused";
    case ExportPhase::Done:
        return "done";
    case ExportPhase::Failed:
        return "failed";
    case ExportPhase::Cancelled:
        return "cancelled";
    }
    return "queued";
}

Json statusJson(const std::string& job, const ExportJobStatus& s) {
    Json j{{"job", job},
           {"output", pathToUtf8(s.output)},
           {"preset", s.presetId},
           {"state", jobState(s)},
           {"progress", s.progress.phase == ExportPhase::Done ? 1.0 : s.progress.fraction()}};
    if (s.result) {
        j["detail"] = std::format("{} frames, {:.1f} s, {:.1f}x realtime", s.result->frames, s.result->seconds,
                                  s.result->realtimeFactor);
        j["output"] = pathToUtf8(s.result->output);
        j["bytes"] = s.result->bytes;
        if (!s.result->warnings.empty()) {
            j["warnings"] = s.result->warnings;
        }
        if (s.result->copiedFrames > 0) {
            j["copiedFrames"] = s.result->copiedFrames;
        }
    } else if (s.error) {
        j["detail"] = s.error->message;
    }
    return j;
}

std::filesystem::path defaultOutputFor(const std::filesystem::path& projectFile, const Sequence& sequence,
                                       const ExportSettings& settings) {
    std::error_code ec;
    std::filesystem::path folder = projectFile.empty() ? std::filesystem::current_path(ec) : projectFile.parent_path();
    std::string base = sequence.name.empty() ? std::string("Export") : sequence.name;
    for (char& ch : base) {
        if (std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos || static_cast<unsigned char>(ch) < 32) {
            ch = '_';
        }
    }
    const std::string ext(outputExtension(settings));
    std::filesystem::path path = folder / pathFromUtf8(base + ext);
    for (int n = 2; std::filesystem::exists(path, ec) && n < 1000; ++n) {
        path = folder / pathFromUtf8(std::format("{} ({}){}", base, n, ext));
    }
    return path;
}

} // namespace

// ---- Standalone host (wabcut-cli, MCP) ------------------------------------------------------------------------

struct ExportCommandServices::Standalone {
    std::mutex mutex;
    std::map<std::string, ExportJobId> ids;
    std::vector<std::string> order;
    std::atomic<std::uint64_t> next{1};
    ExportQueue queue;
    std::shared_ptr<WatchFolderService> watch; // after the queue: destroyed first
};

ExportCommandServices::ExportCommandServices(Host host) : m_host(std::move(host)) {}

std::shared_ptr<ExportCommandServices> ExportCommandServices::standalone(std::filesystem::path projectFile,
                                                                         std::filesystem::path presetFolder,
                                                                         VideoSourceFactory videoSource) {
    auto state = std::make_shared<Standalone>();
    WatchFolderService::Options watchOptions;
    watchOptions.videoSource = videoSource;
    state->watch = std::make_shared<WatchFolderService>(state->queue, watchOptions);
    Host host;
    std::weak_ptr<Standalone> weak = state;
    host.newJobId = [weak] {
        auto s = weak.lock();
        return s ? std::format("j{}", s->next++) : std::string("j0");
    };
    host.enqueue = [weak](std::string job, ExportRequest request, std::string, bool hold) {
        if (auto s = weak.lock()) {
            const ExportJobId id = s->queue.enqueue(std::move(request), hold);
            std::lock_guard lock(s->mutex);
            s->ids[job] = id;
            s->order.push_back(job);
        }
    };
    host.jobs = [weak] {
        Json out = Json::array();
        if (auto s = weak.lock()) {
            std::lock_guard lock(s->mutex);
            for (const std::string& job : s->order) {
                if (const auto status = s->queue.status(s->ids[job])) {
                    out.push_back(statusJson(job, *status));
                }
            }
        }
        return out;
    };
    host.control = [weak](std::string_view job, std::string_view action) -> std::expected<void, Error> {
        auto s = weak.lock();
        if (!s) {
            return std::unexpected(Error{ErrorCode::NotFound, "no export queue"});
        }
        ExportJobId id = 0;
        {
            std::lock_guard lock(s->mutex);
            const auto it = s->ids.find(std::string(job));
            if (it == s->ids.end()) {
                return std::unexpected(Error{ErrorCode::NotFound, std::format("No export job \"{}\"", job)});
            }
            id = it->second;
        }
        if (action == "start") {
            s->queue.start(id);
        } else if (action == "pause") {
            s->queue.pause(id);
        } else if (action == "resume") {
            s->queue.resume(id);
        } else if (action == "cancel") {
            s->queue.cancel(id);
        } else if (action == "remove") {
            s->queue.remove(id);
            std::lock_guard lock(s->mutex);
            if (!s->queue.status(id)) {
                s->ids.erase(std::string(job));
                std::erase(s->order, std::string(job));
            }
        } else {
            return std::unexpected(Error{ErrorCode::InvalidArgument, std::format("Unknown action \"{}\"", action)});
        }
        return {};
    };
    host.projectFile = projectFile;
    host.videoSource = std::move(videoSource);
    if (!presetFolder.empty()) {
        host.presets = std::make_shared<PresetStore>(std::move(presetFolder));
    }
    host.watch = state->watch;
    auto services = std::make_shared<ExportCommandServices>(std::move(host));
    services->m_standalone = std::move(state);
    return services;
}

Json ExportCommandServices::waitForJobs() {
    if (!m_standalone) {
        return jobs();
    }
    std::vector<std::pair<std::string, ExportJobId>> all;
    {
        std::lock_guard lock(m_standalone->mutex);
        for (const std::string& job : m_standalone->order) {
            all.emplace_back(job, m_standalone->ids[job]);
        }
    }
    Json out = Json::array();
    for (const auto& [job, id] : all) {
        // A held job never starts by itself: it is reported as it is.
        const auto now = m_standalone->queue.status(id);
        const auto status = now && now->held ? now : m_standalone->queue.wait(id);
        if (status) {
            out.push_back(statusJson(job, *status));
        }
    }
    return out;
}

// ---- Presets ------------------------------------------------------------------------------------------------

Json ExportCommandServices::presetJson(const Preset& p, bool builtIn) {
    return Json{{"id", p.id},
                {"name", p.name},
                {"description", p.description},
                {"builtIn", builtIn},
                {"container", containerName(p.settings.container)},
                {"videoCodec", p.settings.video.enabled ? Json(videoCodecName(p.settings.video.codec)) : Json(nullptr)},
                {"extension", outputExtension(p.settings)},
                {"settings", settingsToJson(p.settings)}};
}

Json ExportCommandServices::presets() const {
    Json out = Json::array();
    for (const Preset& p : builtInPresets()) {
        out.push_back(presetJson(p, true));
    }
    if (m_host.presets) {
        for (const Preset& p : m_host.presets->list()) {
            out.push_back(presetJson(p, false));
        }
    }
    return out;
}

std::expected<std::pair<Preset, ExportSettings>, Error> ExportCommandServices::settingsFor(const std::string& preset,
                                                                                           const Json& overrides) const {
    std::optional<Preset> p = m_host.presets ? m_host.presets->findAny(preset) : findPreset(preset);
    if (!p) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::format("Unknown preset \"{}\"", preset)});
    }
    ExportSettings s = p->settings;
    if (auto ok = applyOverrides(s, overrides); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = validate(s); !ok) {
        return std::unexpected(ok.error());
    }
    return std::pair<Preset, ExportSettings>{std::move(*p), std::move(s)};
}

std::expected<ExportRequest, command::CommandError>
ExportCommandServices::requestFor(std::shared_ptr<const ProjectState> state, SequenceId sequenceId, const Output& output,
                                  std::string* presetName) const {
    const Sequence* sequence = state ? state->findSequence(sequenceId) : nullptr;
    if (!sequence) {
        return std::unexpected(command::CommandError{command::ErrorCode::NotFound, "no such sequence", nullptr});
    }
    auto resolved = settingsFor(output.preset, output.overrides);
    if (!resolved) {
        return std::unexpected(commandError(resolved.error()));
    }
    if (auto r = resolve(resolved->second, *sequence, state->colorManagement); !r) {
        return std::unexpected(commandError(r.error()));
    }
    ExportRequest request;
    request.settings = std::move(resolved->second);
    std::filesystem::path out = output.output.empty()
                                    ? (m_host.defaultOutput ? m_host.defaultOutput(*sequence, request.settings)
                                                            : defaultOutputFor(m_host.projectFile, *sequence, request.settings))
                                    : pathFromUtf8(output.output);
    out.replace_extension(std::string(outputExtension(request.settings))); // the format decides
    request.output = std::move(out);
    request.snapshot = std::move(state);
    request.sequence = sequenceId;
    request.projectFile = m_host.projectFile;
    request.videoSource = m_host.videoSource;
    if (request.settings.render.usePreviews) {
        request.previews = m_host.previews;
    }
    if (presetName) {
        *presetName = resolved->first.name;
    }
    return request;
}

std::expected<Json, command::CommandError> ExportCommandServices::describe(const ProjectState& state, SequenceId sequence,
                                                                           const Output& output) const {
    auto snapshot = std::make_shared<const ProjectState>(state);
    std::string presetName;
    auto request = requestFor(snapshot, sequence, output, &presetName);
    if (!request) {
        return std::unexpected(request.error());
    }
    const Sequence& seq = *snapshot->findSequence(sequence);
    const auto r = resolve(request->settings, seq, state.colorManagement);
    Json d{{"preset", output.preset},
           {"presetName", presetName},
           {"output", pathToUtf8(request->output)},
           {"extension", outputExtension(request->settings)},
           {"settings", settingsToJson(request->settings)},
           {"range",
            {{"start", r->range.start.value},
             {"end", r->range.end().value},
             {"in", ticksToTimecode(r->range.start, seq.frameRate, seq.dropFrame)},
             {"out", ticksToTimecode(r->range.end(), seq.frameRate, seq.dropFrame)}}},
           {"duration", r->duration.toSeconds()}};
    if (request->settings.video.enabled) {
        d["width"] = r->width;
        d["height"] = r->height;
        d["frameRate"] = std::format("{}/{}", r->frameRate.num, r->frameRate.den);
        d["frames"] = r->frameCount;
        d["alpha"] = r->alpha;
    }
    if (request->settings.audio.enabled) {
        d["audioSamples"] = r->audioSamples;
    }
    return d;
}

std::vector<std::string> ExportCommandServices::reserveJobs(std::size_t count) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(m_host.newJobId ? m_host.newJobId() : std::format("j{}", i + 1));
    }
    return out;
}

void ExportCommandServices::queue(std::shared_ptr<const ProjectState> state, SequenceId sequence, std::vector<Output> outputs,
                                  std::vector<std::string> jobIds, bool hold) {
    if (!m_host.enqueue) {
        return;
    }
    for (std::size_t i = 0; i < outputs.size() && i < jobIds.size(); ++i) {
        std::string presetName;
        auto request = requestFor(state, sequence, outputs[i], &presetName);
        if (request) {
            m_host.enqueue(jobIds[i], std::move(*request), std::move(presetName), hold);
        }
    }
}

Json ExportCommandServices::jobs() const {
    return m_host.jobs ? m_host.jobs() : Json::array();
}

std::expected<void, command::CommandError> ExportCommandServices::control(std::string_view job, std::string_view action) {
    if (!m_host.control) {
        return std::unexpected(command::CommandError{command::ErrorCode::NotFound, "no export queue", nullptr});
    }
    if (auto ok = m_host.control(job, action); !ok) {
        return std::unexpected(commandError(ok.error()));
    }
    return {};
}

std::expected<Json, command::CommandError> ExportCommandServices::savePreset(const std::string& name,
                                                                             const std::string& description,
                                                                             const Output& from, bool overwrite, bool dryRun) {
    if (!m_host.presets) {
        return std::unexpected(command::CommandError{command::ErrorCode::InvalidRequest, "this host keeps no user presets", nullptr});
    }
    auto resolved = settingsFor(from.preset, from.overrides);
    if (!resolved) {
        return std::unexpected(commandError(resolved.error()));
    }
    if (dryRun) {
        Preset p{userPresetId(name), name, description, resolved->second};
        p.settings.presetId = p.id;
        Json j = presetJson(p, false);
        j["saved"] = false;
        return j;
    }
    auto saved = m_host.presets->save(name, description, std::move(resolved->second), overwrite);
    if (!saved) {
        return std::unexpected(commandError(saved.error()));
    }
    if (m_host.presetsChanged) {
        m_host.presetsChanged();
    }
    Json j = presetJson(*saved, false);
    j["saved"] = true;
    return j;
}

std::expected<Json, command::CommandError> ExportCommandServices::importPreset(const std::string& file, bool overwrite,
                                                                               bool dryRun) {
    if (!m_host.presets) {
        return std::unexpected(command::CommandError{command::ErrorCode::InvalidRequest, "this host keeps no user presets", nullptr});
    }
    if (dryRun) {
        std::error_code ec;
        const std::filesystem::path path = pathFromUtf8(file);
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxPresetFileBytes) {
            return std::unexpected(command::CommandError{command::ErrorCode::NotFound, "cannot read " + file, nullptr});
        }
        return Json{{"file", file}, {"saved", false}};
    }
    auto imported = m_host.presets->importFile(pathFromUtf8(file), overwrite);
    if (!imported) {
        return std::unexpected(commandError(imported.error()));
    }
    if (m_host.presetsChanged) {
        m_host.presetsChanged();
    }
    Json j = presetJson(*imported, false);
    j["saved"] = true;
    return j;
}

std::expected<void, command::CommandError> ExportCommandServices::exportPreset(const std::string& id, const std::string& file,
                                                                               bool dryRun) {
    const std::optional<Preset> p = m_host.presets ? m_host.presets->findAny(id) : findPreset(id);
    if (!p) {
        return std::unexpected(command::CommandError{command::ErrorCode::NotFound, "no preset \"" + id + "\"", nullptr});
    }
    if (dryRun) {
        return {};
    }
    if (auto wrote = io::writeFileAtomically(pathFromUtf8(file), presetToJson(*p)); !wrote) {
        return std::unexpected(commandError(Error{ErrorCode::IoError, wrote.error().message}));
    }
    return {};
}

std::expected<void, command::CommandError> ExportCommandServices::deletePreset(const std::string& id, bool dryRun) {
    if (!m_host.presets) {
        return std::unexpected(command::CommandError{command::ErrorCode::InvalidRequest, "this host keeps no user presets", nullptr});
    }
    if (dryRun) {
        if (!m_host.presets->find(id)) {
            return std::unexpected(command::CommandError{command::ErrorCode::NotFound, "no user preset \"" + id + "\"", nullptr});
        }
        return {};
    }
    if (auto ok = m_host.presets->remove(id); !ok) {
        return std::unexpected(commandError(ok.error()));
    }
    if (m_host.presetsChanged) {
        m_host.presetsChanged();
    }
    return {};
}

// ---- Watch folders --------------------------------------------------------------------------------------------

Json ExportCommandServices::watchFolders() const {
    Json out = Json::array();
    if (!m_host.watch) {
        return out;
    }
    for (const WatchFolderStatus& f : m_host.watch->folders()) {
        out.push_back(Json{{"id", std::format("w{}", f.id)},
                           {"folder", pathToUtf8(f.folder.folder)},
                           {"preset", f.folder.presetId},
                           {"output", pathToUtf8(f.folder.output.empty() ? f.folder.folder / "Output" : f.folder.output)},
                           {"moveSources", f.folder.moveSources},
                           {"queued", f.queued},
                           {"done", f.done},
                           {"failed", f.failed},
                           {"lastError", f.lastError}});
    }
    return out;
}

std::expected<Json, command::CommandError> ExportCommandServices::addWatchFolder(const std::string& folder,
                                                                                 const std::string& preset,
                                                                                 const std::string& output, bool moveSources,
                                                                                 bool dryRun) {
    if (!m_host.watch) {
        return std::unexpected(command::CommandError{command::ErrorCode::InvalidRequest, "this host has no watch folders", nullptr});
    }
    auto resolved = settingsFor(preset, Json::object());
    if (!resolved) {
        return std::unexpected(commandError(resolved.error()));
    }
    WatchFolder w;
    w.folder = pathFromUtf8(folder);
    w.presetId = preset;
    w.settings = std::move(resolved->second);
    w.output = output.empty() ? std::filesystem::path() : pathFromUtf8(output);
    w.moveSources = moveSources;
    const std::string outputText = pathToUtf8(w.output.empty() ? w.folder / "Output" : w.output);
    if (dryRun) {
        return Json{{"folder", folder}, {"preset", preset}, {"output", outputText}, {"moveSources", moveSources}, {"added", false}};
    }
    auto id = m_host.watch->add(std::move(w));
    if (!id) {
        return std::unexpected(commandError(id.error()));
    }
    if (m_host.watchFoldersChanged) {
        m_host.watchFoldersChanged();
    }
    return Json{{"id", std::format("w{}", *id)}, {"folder", folder}, {"preset", preset}, {"output", outputText},
                {"moveSources", moveSources}, {"added", true}};
}

std::expected<void, command::CommandError> ExportCommandServices::removeWatchFolder(std::string_view id, bool dryRun) {
    if (!m_host.watch) {
        return std::unexpected(command::CommandError{command::ErrorCode::InvalidRequest, "this host has no watch folders", nullptr});
    }
    std::uint64_t number = 0;
    const std::string text(id.starts_with("w") ? id.substr(1) : id);
    try {
        number = std::stoull(text);
    } catch (...) {
        return std::unexpected(command::CommandError{command::ErrorCode::NotFound, "no watch folder \"" + std::string(id) + "\"", nullptr});
    }
    if (dryRun) {
        return {};
    }
    if (!m_host.watch->remove(number)) {
        return std::unexpected(command::CommandError{command::ErrorCode::NotFound, "no watch folder \"" + std::string(id) + "\"", nullptr});
    }
    if (m_host.watchFoldersChanged) {
        m_host.watchFoldersChanged();
    }
    return {};
}

} // namespace wabcut::exporter
