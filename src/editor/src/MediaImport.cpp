#include <wabcut/editor/MediaImport.h>

#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace wabcut::editor {

namespace {

std::string lowerExtension(const std::filesystem::path& file) {
    std::string ext = pathToUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::filesystem::path absolutePath(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(file, ec);
    return ec ? file : p.lexically_normal();
}

/// The item of `state` that already holds `item`'s file (same absolute path and content).
const MediaItem* findSameFile(const ProjectState& state, const MediaItem& item) {
    for (const MediaPtr& existing : state.media) {
        if (existing->locator.path == item.locator.path &&
            (!existing->locator.fingerprint.isKnown() ||
             existing->locator.fingerprint.sameContentAs(item.locator.fingerprint))) {
            return existing.get();
        }
    }
    return nullptr;
}

} // namespace

ImportFileKind classifyImportFile(const std::filesystem::path& file) {
    if (lowerExtension(file) == io::kProjectExtension) {
        return ImportFileKind::Project;
    }
    const interchange::Detection detection = interchange::ImportRouter::detect(file);
    return detection.format == interchange::Format::Unknown ? ImportFileKind::Media : ImportFileKind::Interchange;
}

std::expected<MediaItem, Error> describeMediaFile(const std::filesystem::path& file,
                                                  const std::filesystem::path& projectFile) {
    const std::filesystem::path absolute = absolutePath(file);
    auto info = media::probe(pathToUtf8(absolute));
    if (!info) {
        return std::unexpected(std::move(info.error()));
    }
    auto fingerprint = fingerprintFile(absolute);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    MediaItem item;
    item.name = pathToUtf8(absolute.filename());
    item.locator.path = pathToUtf8(absolute);
    item.locator.relativePath = projectFile.empty() ? std::string{} : relativeMediaPath(projectFile, absolute);
    item.locator.fingerprint = std::move(*fingerprint);
    item.info = std::move(*info);
    return item;
}

MediaImportResult importMediaFiles(EditSession& session, std::span<const std::filesystem::path> files,
                                   const MediaImportOptions& options) {
    // Probe in parallel (headers only; the files may sit on a slow disk), commit once on this thread.
    std::vector<std::optional<std::expected<MediaItem, Error>>> probed(files.size());
    const int threads = std::clamp(options.threads > 0 ? options.threads : 4, 1,
                                   std::max(1, static_cast<int>(files.size())));
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (std::size_t i = next.fetch_add(1); i < files.size(); i = next.fetch_add(1)) {
            probed[i] = describeMediaFile(files[i], options.projectFile);
        }
    };
    {
        std::vector<std::jthread> pool;
        for (int t = 1; t < threads; ++t) {
            pool.emplace_back(work);
        }
        work();
    }

    MediaImportResult result;
    result.byFile.resize(files.size());
    std::vector<MediaItem> toAdd;
    std::vector<std::size_t> order; // index in `files` per toAdd entry
    const ProjectSnapshot snapshot = session.snapshot();
    for (std::size_t i = 0; i < files.size(); ++i) {
        auto& outcome = *probed[i];
        if (!outcome) {
            result.failed.push_back({files[i], std::move(outcome.error())});
            continue;
        }
        if (const MediaItem* existing = findSameFile(*snapshot, *outcome)) {
            result.alreadyInProject.push_back(existing->id);
            result.byFile[i] = existing->id;
            continue;
        }
        const bool duplicate = std::ranges::any_of(
            toAdd, [&](const MediaItem& m) { return m.locator.path == outcome->locator.path; });
        if (duplicate) {
            continue; // the same file twice in one import (byFile: below, from the first)
        }
        outcome->bin = options.bin;
        toAdd.push_back(std::move(*outcome));
        order.push_back(i);
    }
    if (toAdd.empty()) {
        return result;
    }

    std::vector<MediaId> ids;
    auto committed = session.apply("Import", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        ids.clear();
        for (const MediaItem& item : toAdd) {
            auto added = edit::addMedia(next, item);
            if (!added) {
                return std::unexpected(std::move(added.error()));
            }
            ids.push_back(added->id);
            next = std::move(added->state);
        }
        return next;
    });
    if (!committed) {
        for (std::size_t i : order) {
            result.failed.push_back({files[i], committed.error()});
        }
        return result;
    }
    for (std::size_t k = 0; k < order.size() && k < ids.size(); ++k) {
        result.byFile[order[k]] = ids[k];
    }
    // The same file given twice: the item of its first mention.
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (result.byFile[i] || !*probed[i]) {
            continue;
        }
        for (std::size_t k = 0; k < order.size() && k < ids.size(); ++k) {
            if (toAdd[k].locator.path == (*probed[i])->locator.path) {
                result.byFile[i] = ids[k];
                break;
            }
        }
    }
    result.added = std::move(ids);
    return result;
}

interchange::MediaInspector probeInspector() {
    return [](const std::filesystem::path& file) -> std::optional<interchange::MediaFacts> {
        auto info = media::probe(pathToUtf8(file));
        if (!info) {
            return std::nullopt;
        }
        interchange::MediaFacts facts;
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (!ec) {
            facts.fileSize = static_cast<std::int64_t>(size);
        }
        if (info->duration > Ticks{}) {
            facts.duration = info->duration;
        }
        return facts;
    };
}

std::expected<interchange::ImportResult, Error> importTimelineFile(EditSession& session,
                                                                   const std::filesystem::path& file,
                                                                   const TimelineImportOptions& options) {
    interchange::ImportOptions importOptions;
    importOptions.mediaSearchRoots = options.mediaSearchRoots;
    importOptions.mediaInspector = probeInspector();
    importOptions.projectFile = options.projectFile;
    importOptions.experimentalPremiereProject = options.experimentalPremiereProject;
    importOptions.targetProject = session.stateForSave();
    for (const SequenceId sequence : options.dropSequences) {
        if (auto without = edit::removeSequence(*importOptions.targetProject, sequence)) {
            importOptions.targetProject = std::move(*without);
        }
    }

    auto imported = interchange::ImportRouter::importFile(file, importOptions);
    if (!imported) {
        return std::unexpected(std::move(imported.error()));
    }
    // Interchange files only declare their media; give every file that was found its probed streams and
    // fingerprint, so playback opens the right streams and relinking can verify it later. Kept only when the
    // timeline still fits the real files (Strict validation), else the declared info stays.
    ProjectState refreshed = imported->project;
    bool changed = false;
    for (MediaPtr& media : refreshed.media) {
        if (media->locator.offline || std::ranges::find(imported->media, media->id) == imported->media.end()) {
            continue;
        }
        auto described = describeMediaFile(pathFromUtf8(media->locator.path), options.projectFile);
        if (!described) {
            continue;
        }
        auto item = std::make_shared<MediaItem>(*media);
        item->info = std::move(described->info);
        item->locator.fingerprint = std::move(described->locator.fingerprint);
        if (item->locator.relativePath.empty()) {
            item->locator.relativePath = std::move(described->locator.relativePath);
        }
        media = std::move(item);
        changed = true;
    }
    const std::string label = "Import " + pathToUtf8(file.filename());
    if (changed && validate(refreshed, Validation::Strict) && session.commit(label, refreshed)) {
        imported->project = std::move(refreshed);
        return imported;
    }
    if (auto committed = session.commit(label, imported->project); !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return imported;
}

} // namespace wabcut::editor
