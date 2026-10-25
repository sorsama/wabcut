#include <wabcut/editor/MediaImport.h>

#include <wabcut/editor/CameraCards.h>
#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/interchange/Xmp.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
// 0.4 (fix round 1): kernel32, declared as the SDK declares it (no <windows.h> in this file: its macros).
extern "C" __declspec(dllimport) unsigned long __stdcall GetFileAttributesW(const wchar_t* lpFileName);
#endif

namespace wabcut::editor {

namespace fs = std::filesystem;

namespace {

std::string lowerExtension(const std::filesystem::path& file) {
    std::string ext = pathToUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string lowerAscii(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// ---- 0.4: Import Folder ----

/// A timeline bundle folder (Final Cut Pro, OpenTimelineIO): imported as a timeline, never expanded.
bool isTimelineBundle(const fs::path& folder) {
    const std::string ext = lowerExtension(folder);
    return ext == ".fcpxmld" || ext == ".otiod" || ext == ".fcpbundle";
}

/// A link of any kind (symbolic link; on Windows also a junction), from its own status (never followed).
bool isLink(const fs::directory_entry& entry) {
    std::error_code ec;
    const fs::file_status status = entry.symlink_status(ec);
    if (ec) {
        return false;
    }
#if defined(_MSC_VER)
    if (status.type() == fs::file_type::junction) {
        return true;
    }
#endif
    return fs::is_symlink(status);
}

/// Hidden or system entries the Media Browser leaves out too: a leading "." (or "$": $RECYCLE.BIN), System Volume
/// Information.
bool isHiddenName(const std::string& name) {
    return name.empty() || name.front() == '.' || name.front() == '$' || lowerAscii(name) == "system volume information";
}

/// Fix round 1: what Explorer hides on Windows - an entry with the hidden attribute, or a file with the system one
/// (desktop.ini, Thumbs.db) - is left out as hidden, not counted as "not media". A folder with the system attribute
/// alone (a customised folder: its desktop.ini sets it) is shown by Explorer, so it is looked in.
bool hasHiddenAttribute(const fs::path& path, bool folder) {
#ifdef _WIN32
    constexpr unsigned long kInvalid = 0xFFFFFFFFUL; // INVALID_FILE_ATTRIBUTES
    constexpr unsigned long kHidden = 0x2;           // FILE_ATTRIBUTE_HIDDEN
    constexpr unsigned long kSystem = 0x4;           // FILE_ATTRIBUTE_SYSTEM
    const unsigned long attributes = GetFileAttributesW(path.c_str());
    return attributes != kInvalid && ((attributes & kHidden) != 0 || (!folder && (attributes & kSystem) != 0));
#else
    (void)path;
    (void)folder;
    return false;
#endif
}

/// The folder's own name; a drive's root ("D:\") gives its drive name ("D").
std::string folderName(const fs::path& folder) {
    std::string name = pathToUtf8(folder.filename());
    if (name.empty()) {
        name = pathToUtf8(folder.root_name());
        std::erase(name, ':');
    }
    return name.empty() ? std::string("Folder") : name;
}

struct FolderWalk {
    const FolderImportLimits& limits;
    FolderImportPlan& plan;
    std::size_t examined = 0;
};

/// `dir`'s media (its files by name, then its subfolders by name, recursively) into the plan, in bins `bins`.
void walkFolder(const fs::path& dir, std::vector<std::string>& bins, int depth, FolderWalk& walk) {
    std::vector<std::pair<std::string, fs::path>> files;   // (sort key, path)
    std::vector<std::pair<std::string, fs::path>> folders;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        const fs::path path = entry.path();
        const std::string name = pathToUtf8(path.filename());
        if (isLink(entry)) {
            walk.plan.skipped.push_back({path, FolderSkip::Link});
            continue;
        }
        std::error_code typeError;
        const bool folder = entry.is_directory(typeError);
        if (isHiddenName(name) || hasHiddenAttribute(path, folder)) {
            walk.plan.skipped.push_back({path, FolderSkip::Hidden});
            continue;
        }
        if (folder) {
            folders.emplace_back(lowerAscii(name), path);
        } else {
            files.emplace_back(lowerAscii(name), path);
        }
    }
    const auto byName = [](const auto& a, const auto& b) { return a.first < b.first; };
    std::ranges::sort(files, byName);
    std::ranges::sort(folders, byName);
    for (const auto& [key, file] : files) {
        if (walk.examined >= walk.limits.maxFiles) {
            walk.plan.truncated = true;
            return;
        }
        ++walk.examined;
        if (!isBrowsableMedia(file)) {
            walk.plan.skipped.push_back({file, FolderSkip::NotMedia});
            continue;
        }
        walk.plan.files.push_back({file, bins});
    }
    for (const auto& [key, folder] : folders) {
        if (isTimelineBundle(folder)) {
            walk.plan.skipped.push_back({folder, FolderSkip::TimelineBundle});
            continue;
        }
        // A camera card's root (the Media Browser imports its clips joined); a card around `dir` was caught before.
        if (const auto card = cameraCardAt(folder); card && card->second.lexically_normal() == folder.lexically_normal()) {
            walk.plan.skipped.push_back({folder, FolderSkip::CameraCard});
            continue;
        }
        if (depth >= walk.limits.maxDepth) {
            walk.plan.skipped.push_back({folder, FolderSkip::TooDeep});
            continue;
        }
        bins.push_back(pathToUtf8(folder.filename()));
        walkFolder(folder, bins, depth + 1, walk);
        bins.pop_back();
        if (walk.plan.truncated) {
            return;
        }
    }
}

// ---- 0.4: capture folders ----

/// "Key=Value" lines by "[Section]" of an INI file read as UTF-8 (BOM skipped); keys and sections as written.
std::map<std::string, std::map<std::string, std::string>> readIni(const fs::path& file) {
    std::map<std::string, std::map<std::string, std::string>> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return out;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
            s.remove_suffix(1);
        }
        return std::string(s);
    };
    std::string section;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        out[section][trim(std::string_view(trimmed).substr(0, eq))] = trim(std::string_view(trimmed).substr(eq + 1));
    }
    return out;
}

/// A path as written in an INI file (either slash kind) as a normalised path, without a trailing separator.
fs::path iniPath(std::string value) {
    std::ranges::replace(value, '\\', '/');
    fs::path p = pathFromUtf8(value).lexically_normal();
    if (p.has_relative_path() && p.filename().empty()) {
        p = p.parent_path();
    }
    return p;
}

/// The key two paths are the same folder by (case-insensitive on Windows).
std::string folderKey(const fs::path& path) {
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    if (ec) {
        p = path;
    }
    p = p.lexically_normal();
    if (p.has_relative_path() && p.filename().empty()) {
        p = p.parent_path();
    }
    std::string key = pathToUtf8(p);
    std::ranges::replace(key, '\\', '/');
#ifdef _WIN32
    key = lowerAscii(std::move(key));
#endif
    return key;
}

#ifdef _WIN32
fs::path environmentPath(const wchar_t* name) {
    wchar_t* value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    fs::path out(value);
    std::free(value);
    return out;
}
#else
fs::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? fs::path(value) : fs::path();
}
#endif

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
    // 0.4: a folder is a timeline bundle (.fcpxmld, .otiod, .fcpbundle: the router reads or explains them, also when
    // their content is missing) or a folder to expand.
    if (std::error_code ec; fs::is_directory(file, ec)) {
        return isTimelineBundle(file) ? ImportFileKind::Interchange : ImportFileKind::Folder;
    }
    if (lowerExtension(file) == io::kProjectExtension) {
        return ImportFileKind::Project;
    }
    // P.2: caption files become caption tracks (Premiere imports them as captions too). An .xml is one only when it is
    // Timed Text (TTML, DFXP, SMPTE-TT), not a timeline.
    const std::string name = pathToUtf8(file.filename());
    if (interchange::hasCaptionFileExtension(name)) {
        return ImportFileKind::Captions;
    }
    if (lowerExtension(file) == ".xml") {
        std::ifstream in(file, std::ios::binary);
        std::string head(64 * 1024, '\0');
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
        if (interchange::detectCaptionFile(head, name)) {
            return ImportFileKind::Captions;
        }
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
            // 0.2 (P.3): the file's XMP fills the linked clip metadata (a file without any keeps it empty).
            if (options.readXmp && *probed[i] && !(*probed[i])->isGenerator()) {
                interchange::XmpReadOptions read;
                read.scanFilesUpTo = std::uintmax_t{32} * 1024 * 1024;
                if (auto packet = interchange::readMediaXmp(files[i], read); packet && *packet) {
                    interchange::applyXmpClipFields(**probed[i], interchange::clipFieldsFromXmp((*packet)->properties), false);
                }
            }
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
        outcome->scaling = options.scaling; // P.2: Preferences > Media > Default Media Scaling
        if (outcome->info.isStillImage && options.stillDuration) {
            outcome->stillDuration = options.stillDuration; // P.2: Preferences > Timeline > Still Image Default Duration
        }
        toAdd.push_back(std::move(*outcome));
        order.push_back(i);
    }
    if (toAdd.empty()) {
        return result;
    }

    std::vector<MediaId> ids;
    std::vector<BinId> createdBins;
    const std::string label = options.undoLabel.empty() ? std::string("Import") : options.undoLabel;
    auto committed = session.apply(label, [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        ids.clear();
        createdBins.clear();
        // The target bin: an unknown one is the root, a search bin (it holds nothing) its parent.
        BinId target = options.bin;
        if (const Bin* bin = target.isValid() ? next.findBin(target) : nullptr; !bin) {
            target = BinId{};
        } else if (bin->isSearch()) {
            target = bin->parent;
        }
        // 0.4: each file's bins under the target (binPaths), reusing a child bin of the same name, made only for items
        // that are added.
        const auto binFor = [&](std::size_t file) -> std::expected<BinId, Error> {
            BinId bin = target;
            if (file >= options.binPaths.size()) {
                return bin;
            }
            for (const std::string& name : options.binPaths[file]) {
                const auto existing = std::ranges::find_if(
                    next.bins, [&](const Bin& b) { return b.parent == bin && b.name == name && !b.isSearch(); });
                if (existing != next.bins.end()) {
                    bin = existing->id;
                    continue;
                }
                auto made = edit::addBin(next, name, bin);
                if (!made) {
                    return std::unexpected(std::move(made.error()));
                }
                next = std::move(made->state);
                bin = made->id;
                createdBins.push_back(bin);
            }
            return bin;
        };
        for (std::size_t k = 0; k < toAdd.size(); ++k) {
            MediaItem item = toAdd[k];
            auto bin = binFor(order[k]);
            if (!bin) {
                return std::unexpected(std::move(bin.error()));
            }
            item.bin = *bin;
            auto added = edit::addMedia(next, std::move(item));
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
    result.createdBins = std::move(createdBins);
    return result;
}

FolderImportPlan collectImportFolder(const std::filesystem::path& folder, const FolderImportLimits& limits) {
    FolderImportPlan plan;
    std::error_code ec;
    fs::path root = fs::absolute(folder, ec);
    if (ec) {
        root = folder;
    }
    root = root.lexically_normal();
    if (root.has_relative_path() && root.filename().empty()) {
        root = root.parent_path(); // "D:\Footage\" -> "D:\Footage"
    }
    if (isTimelineBundle(root)) {
        plan.skipped.push_back({root, FolderSkip::TimelineBundle});
        return plan;
    }
    // A camera card, or a folder inside one: the Media Browser imports its clips (spanned clips joined).
    if (cameraCardAt(root)) {
        plan.skipped.push_back({root, FolderSkip::CameraCard});
        return plan;
    }
    FolderWalk walk{limits, plan};
    std::vector<std::string> bins{folderName(root)};
    walkFolder(root, bins, 0, walk);
    return plan;
}

std::vector<CaptureFolder> captureFolders(const std::filesystem::path& videos, const std::filesystem::path& obsConfig) {
    std::vector<CaptureFolder> out;
    std::vector<std::string> seen;
    const auto add = [&](CaptureTool tool, std::string profile, const fs::path& path) {
        std::error_code ec;
        if (path.empty() || !fs::is_directory(path, ec)) {
            return;
        }
        std::string key = folderKey(path);
        if (std::ranges::find(seen, key) != seen.end()) {
            return;
        }
        seen.push_back(std::move(key));
        out.push_back({tool, std::move(profile), path});
    };
    if (!videos.empty()) {
        add(CaptureTool::GameBar, {}, videos / "Captures");
        add(CaptureTool::ShadowPlay, {}, videos / "NVIDIA");
    }
    if (obsConfig.empty()) {
        return out;
    }
    // OBS Studio: every profile's recording folder, the one its output mode uses first.
    std::vector<std::pair<std::string, fs::path>> profiles; // (sort key, folder)
    std::error_code ec;
    for (fs::directory_iterator it(obsConfig / "basic" / "profiles", fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code typeError;
        if (it->is_directory(typeError)) {
            profiles.emplace_back(lowerAscii(pathToUtf8(it->path().filename())), it->path());
        }
    }
    std::ranges::sort(profiles, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [key, profileFolder] : profiles) {
        auto ini = readIni(profileFolder / "basic.ini");
        const std::string mode = lowerAscii(ini["Output"]["Mode"]);
        const std::string simple = ini["SimpleOutput"]["FilePath"];
        const std::string advanced = ini["AdvOut"]["RecFilePath"];
        std::string profile = ini["General"]["Name"];
        if (profile.empty()) {
            profile = pathToUtf8(profileFolder.filename());
        }
        std::vector<std::string> candidates = mode == "advanced" ? std::vector<std::string>{advanced, simple}
                                                                 : std::vector<std::string>{simple, advanced};
        for (const std::string& candidate : candidates) {
            if (candidate.empty()) {
                continue;
            }
            const fs::path path = iniPath(candidate);
            std::error_code dirError;
            if (fs::is_directory(path, dirError)) {
                add(CaptureTool::Obs, profile, path);
                break;
            }
        }
    }
    return out;
}

std::filesystem::path obsConfigFolder() {
#ifdef _WIN32
    const fs::path appData = environmentPath(L"APPDATA");
    return appData.empty() ? fs::path() : appData / "obs-studio";
#elif defined(__APPLE__)
    const fs::path home = environmentPath("HOME");
    return home.empty() ? fs::path() : home / "Library" / "Application Support" / "obs-studio";
#else
    if (const fs::path config = environmentPath("XDG_CONFIG_HOME"); !config.empty()) {
        return config / "obs-studio";
    }
    const fs::path home = environmentPath("HOME");
    return home.empty() ? fs::path() : home / ".config" / "obs-studio";
#endif
}

std::expected<MediaItem, Error> describeImageSequence(const std::filesystem::path& firstFrame, Rate frameRate,
                                                      const std::filesystem::path& projectFile) {
    auto sequence = findImageSequence(firstFrame, frameRate);
    if (!sequence) {
        return std::unexpected(std::move(sequence.error()));
    }
    const std::string path = imageSequencePath(*sequence);
    auto info = media::probe(path);
    if (!info) {
        return std::unexpected(std::move(info.error()));
    }
    if (info->video.size() != 1 || !info->audio.empty()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Not an image sequence: " + pathToUtf8(firstFrame)});
    }
    auto fingerprint = fingerprintFile(pathFromUtf8(path));
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    MediaItem item;
    item.name = pathToUtf8(sequence->firstFramePath().filename()); // Premiere names the clip after its first file
    item.locator.path = path;
    item.locator.relativePath = projectFile.empty() ? std::string{} : relativeMediaPath(projectFile, pathFromUtf8(path));
    item.locator.fingerprint = std::move(*fingerprint);
    item.info = std::move(*info);
    return item;
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
