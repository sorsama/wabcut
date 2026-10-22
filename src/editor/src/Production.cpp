// Productions and project locks (Production.h, 0.2 P.3).

#include <wabcut/editor/Production.h>

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace wabcut::editor {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::uintmax_t kMaxSmallFileBytes = 64 * 1024; ///< a production file or a lock

std::optional<std::string> readSmallFile(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec || size > kMaxSmallFileBytes) {
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
        return std::nullopt;
    }
    return bytes;
}

std::string boundedText(const json& value, std::size_t limit) {
    if (!value.is_string()) {
        return {};
    }
    std::string text = value.get<std::string>();
    if (text.size() > limit) {
        text.resize(limit);
    }
    return text;
}

std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    std::string out;
    if (_dupenv_s(&value, &length, name) == 0 && value) {
        out = value;
    }
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

std::string hostName() {
#ifdef _WIN32
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &size)) {
        return pathToUtf8(fs::path(std::wstring(buffer, size)));
    }
    return environment("COMPUTERNAME");
#else
    char buffer[256] = {};
    if (gethostname(buffer, sizeof buffer - 1) == 0) {
        return buffer;
    }
    return environment("HOSTNAME");
#endif
}

std::int64_t processId() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

bool processRunning(std::int64_t pid) {
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED; // it runs, as someone else
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

bool sameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

std::string nowUtc() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

fs::path absoluteNormal(const fs::path& p) {
    std::error_code ec;
    fs::path out = fs::absolute(p, ec);
    return (ec ? p : out).lexically_normal();
}

/// A file or folder name a user gave: not empty, no separators or reserved characters, not "." or "..".
bool isPlainName(std::string_view name) {
    if (name.empty() || name.size() > 200 || name == "." || name == "..") {
        return false;
    }
    return std::none_of(name.begin(), name.end(), [](char c) {
        return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
               c == '|' || static_cast<unsigned char>(c) < 0x20;
    });
}

bool isProjectFile(const fs::path& p) {
    std::string ext = pathToUtf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return ext == io::kProjectExtension;
}

std::expected<void, Error> writeProductionFile(const fs::path& folder, const std::string& name,
                                               const ProductionSettings& settings = {}) {
    json doc{{"format", std::string(kProductionFormat)}, {"version", kProductionVersion}, {"name", name}};
    if (!(settings == ProductionSettings{})) {
        json s = json::object();
        if (!settings.previews.empty()) {
            s["previews"] = settings.previews;
        }
        if (!settings.mediaCache.empty()) {
            s["mediaCache"] = settings.mediaCache;
        }
        doc["settings"] = std::move(s);
    }
    return io::writeFileAtomically(folder / std::string(kProductionFileName),
                                   doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
}

} // namespace

// ---- Locks --------------------------------------------------------------------------------------------------

std::string ProjectLock::owner() const {
    if (user.empty()) {
        return host;
    }
    return host.empty() ? user : user + " on " + host;
}

fs::path projectLockPath(const fs::path& project) {
    fs::path out = project;
    out += ".lock";
    return out;
}

ProjectLock currentLockHolder() {
    ProjectLock lock;
#ifdef _WIN32
    lock.user = environment("USERNAME");
#else
    lock.user = environment("USER");
#endif
    lock.host = hostName();
    lock.pid = processId();
    lock.since = nowUtc();
    for (std::string* field : {&lock.user, &lock.host}) {
        if (field->size() > ProjectLock::kMaxFieldBytes) {
            field->resize(ProjectLock::kMaxFieldBytes);
        }
    }
    return lock;
}

std::optional<ProjectLock> readProjectLock(const fs::path& project) {
    const auto text = readSmallFile(projectLockPath(project));
    if (!text) {
        return std::nullopt;
    }
    const json doc = json::parse(*text, nullptr, false);
    if (!doc.is_object() || !doc.contains("format") || doc["format"] != "wabcut-project-lock") {
        return std::nullopt;
    }
    ProjectLock lock;
    lock.user = boundedText(doc.value("user", json()), ProjectLock::kMaxFieldBytes);
    lock.host = boundedText(doc.value("host", json()), ProjectLock::kMaxFieldBytes);
    lock.since = boundedText(doc.value("since", json()), 64);
    const json pid = doc.value("pid", json());
    lock.pid = pid.is_number_integer() ? pid.get<std::int64_t>() : 0;
    return lock;
}

bool heldByOther(const ProjectLock& lock) {
    const ProjectLock me = currentLockHolder();
    if (!sameText(lock.host, me.host)) {
        return true; // another computer: it cannot be checked, so it counts
    }
    return lock.pid != me.pid && processRunning(lock.pid);
}

std::expected<std::optional<ProjectLock>, Error> acquireProjectLock(const fs::path& project) {
    if (const auto existing = readProjectLock(project); existing && heldByOther(*existing)) {
        return std::optional<ProjectLock>(*existing);
    }
    const ProjectLock me = currentLockHolder();
    const json doc{{"format", "wabcut-project-lock"},
                   {"user", me.user},
                   {"host", me.host},
                   {"pid", me.pid},
                   {"since", me.since}};
    if (auto written = io::writeFileAtomically(projectLockPath(project), doc.dump(2) + "\n"); !written) {
        return std::unexpected(written.error());
    }
    return std::optional<ProjectLock>{};
}

void releaseProjectLock(const fs::path& project) {
    const auto lock = readProjectLock(project);
    if (!lock) {
        return;
    }
    const ProjectLock me = currentLockHolder();
    if (sameText(lock->host, me.host) && lock->pid == me.pid) {
        std::error_code ec;
        fs::remove(projectLockPath(project), ec);
    }
}

// ---- Productions --------------------------------------------------------------------------------------------

std::expected<Production, Error> Production::create(const fs::path& folderIn, std::string name) {
    const fs::path folder = absoluteNormal(folderIn);
    std::error_code ec;
    if (fs::exists(folder / std::string(kProductionFileName), ec)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, pathToUtf8(folder) + " already is a production"});
    }
    if (const auto outer = folderOf(folder / "probe.wabcut"); outer && *outer != folder) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a production cannot be inside another production (" +
                                                                     pathToUtf8(*outer) + ")"});
    }
    fs::create_directories(folder, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot make the folder " + pathToUtf8(folder)});
    }
    if (name.empty()) {
        name = pathToUtf8(folder.filename());
    }
    if (name.size() > 200) {
        name.resize(200);
    }
    if (auto written = writeProductionFile(folder, name); !written) {
        return std::unexpected(written.error());
    }
    return Production(folder, std::move(name));
}

std::expected<Production, Error> Production::open(const fs::path& folderOrFile) {
    fs::path folder = absoluteNormal(folderOrFile);
    std::error_code ec;
    if (fs::is_regular_file(folder, ec)) {
        folder = folder.parent_path();
    }
    const auto text = readSmallFile(folder / std::string(kProductionFileName));
    if (!text) {
        return std::unexpected(Error{ErrorCode::NotFound, pathToUtf8(folder) + " is not a production"});
    }
    const json doc = json::parse(*text, nullptr, false);
    if (!doc.is_object() || doc.value("format", json()) != std::string(kProductionFormat)) {
        return std::unexpected(Error{ErrorCode::ParseError, pathToUtf8(folder) + " has a broken production file"});
    }
    const json version = doc.value("version", json());
    if (!version.is_number_integer() || version.get<std::int64_t>() > kProductionVersion) {
        return std::unexpected(Error{ErrorCode::VersionMismatch, "the production " + pathToUtf8(folder) +
                                                                     " was made by a newer Wabcut"});
    }
    std::string name = boundedText(doc.value("name", json()), 200);
    if (name.empty()) {
        name = pathToUtf8(folder.filename());
    }
    ProductionSettings settings;
    if (const json s = doc.value("settings", json()); s.is_object()) {
        settings.previews = boundedText(s.value("previews", json()), ProductionSettings::kMaxPathBytes);
        settings.mediaCache = boundedText(s.value("mediaCache", json()), ProductionSettings::kMaxPathBytes);
    }
    return Production(folder, std::move(name), std::move(settings));
}

std::optional<Production> productionOf(const fs::path& projectFile) {
    const auto folder = Production::folderOf(projectFile);
    if (!folder) {
        return std::nullopt;
    }
    auto opened = Production::open(*folder);
    return opened ? std::optional<Production>(std::move(*opened)) : std::nullopt;
}

std::expected<void, Error> Production::setSettings(ProductionSettings settings) {
    for (const std::string* path : {&settings.previews, &settings.mediaCache}) {
        if (path->size() > ProductionSettings::kMaxPathBytes) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "a scratch disk path is too long"});
        }
    }
    if (auto written = writeProductionFile(m_folder, m_name, settings); !written) {
        return std::unexpected(written.error());
    }
    m_settings = std::move(settings);
    return {};
}

fs::path Production::resolve(const std::string& setting) const {
    if (setting.empty()) {
        return {};
    }
    const fs::path p = pathFromUtf8(setting);
    return (p.is_absolute() ? p : m_folder / p).lexically_normal();
}

fs::path Production::previewFolderFor(const fs::path& projectFile) const {
    const fs::path base = resolve(m_settings.previews);
    if (base.empty()) {
        return {};
    }
    fs::path stem = projectFile.stem();
    stem += " Preview Files";
    return base / stem;
}

std::optional<fs::path> Production::folderOf(const fs::path& projectFile) {
    fs::path folder = absoluteNormal(projectFile).parent_path();
    std::error_code ec;
    for (int i = 0; i <= kMaxProductionDepth && !folder.empty(); ++i) {
        if (fs::is_regular_file(folder / std::string(kProductionFileName), ec)) {
            return folder;
        }
        const fs::path up = folder.parent_path();
        if (up == folder) {
            break;
        }
        folder = up;
    }
    return std::nullopt;
}

std::expected<void, Error> Production::rename(std::string name) {
    if (name.empty() || name.size() > 200) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a production needs a name"});
    }
    if (auto written = writeProductionFile(m_folder, name, m_settings); !written) {
        return std::unexpected(written.error());
    }
    m_name = std::move(name);
    return {};
}

bool Production::contains(const fs::path& path) const {
    const fs::path p = absoluteNormal(path);
    auto a = m_folder.begin();
    auto b = p.begin();
    for (; a != m_folder.end(); ++a, ++b) {
        if (b == p.end() || !sameText(pathToUtf8(*a), pathToUtf8(*b))) {
            return false;
        }
    }
    return true;
}

std::vector<ProductionEntry> Production::entries() const {
    std::vector<ProductionEntry> out;
    const std::function<void(const fs::path&, const std::string&, int)> walk = [&](const fs::path& folder,
                                                                                   const std::string& relative, int depth) {
        if (depth >= kMaxProductionDepth) {
            return;
        }
        std::vector<fs::directory_entry> folders;
        std::vector<fs::directory_entry> projects;
        std::error_code ec;
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            const std::string name = pathToUtf8(it->path().filename());
            if (name.empty() || name.front() == '.' || folders.size() + projects.size() + out.size() >= kMaxProductionEntries) {
                continue;
            }
            std::error_code kind;
            if (it->is_directory(kind)) {
                folders.push_back(*it);
            } else if (it->is_regular_file(kind) && isProjectFile(it->path())) {
                projects.push_back(*it); // "x.wabcut" only: autosaves (".autosave"), backups (".bak1") and locks differ
            }
        }
        const auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return pathToUtf8(a.path().filename()) < pathToUtf8(b.path().filename());
        };
        std::sort(folders.begin(), folders.end(), byName);
        std::sort(projects.begin(), projects.end(), byName);
        for (const fs::directory_entry& f : folders) {
            ProductionEntry e;
            e.path = f.path();
            e.name = pathToUtf8(f.path().filename());
            e.relative = relative.empty() ? e.name : relative + "/" + e.name;
            e.folder = true;
            e.depth = depth;
            out.push_back(e);
            walk(f.path(), e.relative, depth + 1);
        }
        for (const fs::directory_entry& p : projects) {
            ProductionEntry e;
            e.path = p.path();
            e.name = pathToUtf8(p.path().stem());
            const std::string file = pathToUtf8(p.path().filename());
            e.relative = relative.empty() ? file : relative + "/" + file;
            e.depth = depth;
            e.lock = readProjectLock(p.path());
            e.lockedByOther = e.lock && heldByOther(*e.lock);
            out.push_back(std::move(e));
        }
    };
    walk(m_folder, {}, 0);
    return out;
}

std::expected<fs::path, Error> Production::folderAt(std::string_view relative) const {
    fs::path folder = m_folder;
    std::string rest(relative);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    std::size_t start = 0;
    int depth = 0;
    while (start < rest.size()) {
        const std::size_t slash = rest.find('/', start);
        const std::string part = rest.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (!isPlainName(part) || ++depth > kMaxProductionDepth) {
                return std::unexpected(Error{ErrorCode::InvalidArgument, "'" + std::string(relative) + "' is not a folder of the production"});
            }
            folder /= pathFromUtf8(part);
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "the production has no folder '" + std::string(relative) + "'"});
    }
    return folder;
}

std::expected<fs::path, Error> Production::newFolder(std::string_view relative, std::string_view name) {
    if (!isPlainName(name)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a folder needs a name without / \\ : * ? \" < > |"});
    }
    auto parent = folderAt(relative);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    const fs::path folder = *parent / pathFromUtf8(std::string(name));
    std::error_code ec;
    if (fs::exists(folder, ec)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "'" + std::string(name) + "' already exists"});
    }
    fs::create_directory(folder, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot make the folder " + pathToUtf8(folder)});
    }
    return folder;
}

std::expected<fs::path, Error> Production::newProject(std::string_view relative, std::string_view name,
                                                      const ProjectState& initial) {
    if (!isPlainName(name)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "a project needs a name without / \\ : * ? \" < > |"});
    }
    auto parent = folderAt(relative);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    fs::path file = *parent / pathFromUtf8(std::string(name));
    if (!isProjectFile(file)) {
        file += std::string(io::kProjectExtension);
    }
    std::error_code ec;
    if (fs::exists(file, ec)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "'" + pathToUtf8(file.filename()) + "' already exists"});
    }
    if (auto saved = io::saveProject(initial, file, io::SaveOptions{0}); !saved) {
        return std::unexpected(saved.error());
    }
    return file;
}

std::expected<fs::path, Error> Production::addProject(const fs::path& project, std::string_view relative) {
    auto parent = folderAt(relative);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    fs::path target = *parent / project.filename();
    std::error_code ec;
    for (int n = 2; fs::exists(target, ec) && n < 1000; ++n) {
        target = *parent / pathFromUtf8(pathToUtf8(project.stem()) + " " + std::to_string(n) + std::string(io::kProjectExtension));
    }
    auto state = loadProjectForImport(project, target);
    if (!state) {
        return std::unexpected(state.error());
    }
    if (auto saved = io::saveProject(*state, target, io::SaveOptions{0}); !saved) {
        return std::unexpected(saved.error());
    }
    return target;
}

std::expected<ProjectState, Error> loadProjectForImport(const fs::path& source, const fs::path& targetProject) {
    auto loaded = io::loadProject(source);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return preparedForImport(*loaded, source, targetProject);
}

ProjectState preparedForImport(const ProjectState& loaded, const fs::path& source, const fs::path& targetProject) {
    const fs::path sourceFile = absoluteNormal(source);
    ProjectState state = withMediaChecked(loaded, sourceFile);
    for (MediaPtr& media : state.media) {
        if (media->isGenerator()) {
            continue;
        }
        // Where the file is (found), or would be by its old relative path (offline): relative to the new project.
        fs::path file = pathFromUtf8(media->locator.path);
        if (media->locator.offline && !media->locator.relativePath.empty()) {
            file = (sourceFile.parent_path() / pathFromUtf8(media->locator.relativePath)).lexically_normal();
        }
        std::string relative;
        if (!targetProject.empty() && !file.empty()) {
            relative = relativeMediaPath(absoluteNormal(targetProject), file);
        }
        if (relative != media->locator.relativePath) {
            auto item = std::make_shared<MediaItem>(*media);
            item->locator.relativePath = std::move(relative);
            media = std::move(item);
        }
    }
    for (ProjectAsset& asset : state.assets) {
        if (!asset.locator.offline && !asset.locator.path.empty()) {
            asset.locator.relativePath =
                targetProject.empty() ? std::string{} : relativeMediaPath(absoluteNormal(targetProject), pathFromUtf8(asset.locator.path));
        }
    }
    return state;
}

} // namespace wabcut::editor
