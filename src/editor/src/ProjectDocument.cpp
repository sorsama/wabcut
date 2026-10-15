#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace wabcut::editor {

namespace {

/// Entries a relink search visits at most (a whole drive is not searched by accident).
constexpr std::size_t kMaxSearchEntries = 250'000;

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::filesystem::path absolutePath(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(file, ec);
    return ec ? file : p.lexically_normal();
}

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

/// `state` with every online item's relative path computed for `projectFile` (what save writes).
ProjectState rebased(ProjectState state, const std::filesystem::path& projectFile) {
    for (MediaPtr& media : state.media) {
        if (media->locator.offline || media->locator.path.empty()) {
            continue; // keep what it had: it may still find the file relative to the project later
        }
        std::string relative = relativeMediaPath(projectFile, pathFromUtf8(media->locator.path));
        if (relative != media->locator.relativePath) {
            auto item = std::make_shared<MediaItem>(*media);
            item->locator.relativePath = std::move(relative);
            media = std::move(item);
        }
    }
    for (ProjectAsset& asset : state.assets) { // LUT files: the same relative paths
        if (!asset.locator.offline && !asset.locator.path.empty()) {
            asset.locator.relativePath = relativeMediaPath(projectFile, pathFromUtf8(asset.locator.path));
        }
    }
    return state;
}

/// "<project>.autosave" (n = 1) or "<project>.autosave<n>".
std::filesystem::path rotatedAutosave(const std::filesystem::path& projectFile, int n) {
    std::filesystem::path p = io::autosavePath(projectFile);
    if (n > 1) {
        p += std::to_string(n);
    }
    return p;
}

/// "Untitled-<id>.wabcut.autosave" (n = 1) or "...autosave<n>": the session's name ("Untitled-<id>.wabcut") and n.
std::optional<std::pair<std::string, int>> untitledAutosaveName(const std::string& name) {
    const std::string marker = std::string(io::kProjectExtension) + ".autosave";
    const std::size_t at = name.rfind(marker);
    if (!name.starts_with("Untitled-") || at == std::string::npos) {
        return std::nullopt;
    }
    const std::string number = name.substr(at + marker.size());
    if (number.size() > 4 || !std::ranges::all_of(number, [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    const int n = number.empty() ? 1 : std::stoi(number);
    if (n < 1) {
        return std::nullopt;
    }
    return std::pair{name.substr(0, at + std::string(io::kProjectExtension).size()), n};
}

std::optional<std::string> readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return std::nullopt;
    }
    return bytes;
}

std::string newUntitledId() {
    static std::atomic<std::uint32_t> counter{0};
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count()) + "-" +
           std::to_string(counter.fetch_add(1) + 1);
}

} // namespace

// ---- Media on disk ------------------------------------------------------------------------------------

std::pair<MediaItem, std::string> checkMediaOnDisk(const MediaItem& item, const std::filesystem::path& projectFile) {
    if (item.isGenerator()) {
        return {item, {}}; // synthetic (a title): no file, never offline
    }
    MediaItem out = item;
    std::vector<std::filesystem::path> candidates;
    if (!item.locator.path.empty()) {
        candidates.push_back(pathFromUtf8(item.locator.path));
    }
    if (!item.locator.relativePath.empty() && !projectFile.empty()) {
        candidates.push_back(
            (absolutePath(projectFile).parent_path() / pathFromUtf8(item.locator.relativePath)).lexically_normal());
    }
    std::string reason = candidates.empty() ? "no path" : "not found";
    for (const std::filesystem::path& candidate : candidates) {
        if (!isFile(candidate)) {
            continue;
        }
        if (item.locator.fingerprint.isKnown()) {
            const auto fingerprint = fingerprintFile(candidate);
            if (!fingerprint || !fingerprint->sameContentAs(item.locator.fingerprint)) {
                reason = "a different file is at " + pathToUtf8(candidate);
                continue;
            }
        }
        out.locator.path = pathToUtf8(absolutePath(candidate));
        out.locator.offline = false;
        return {std::move(out), {}};
    }
    out.locator.offline = true;
    return {std::move(out), std::move(reason)};
}

std::pair<ProjectAsset, std::string> checkAssetOnDisk(const ProjectAsset& asset, const std::filesystem::path& projectFile) {
    ProjectAsset out = asset;
    std::vector<std::filesystem::path> candidates;
    if (!asset.locator.path.empty()) {
        candidates.push_back(pathFromUtf8(asset.locator.path));
    }
    if (!asset.locator.relativePath.empty() && !projectFile.empty()) {
        candidates.push_back((absolutePath(projectFile).parent_path() / pathFromUtf8(asset.locator.relativePath)).lexically_normal());
    }
    std::string reason = candidates.empty() ? "no path" : "not found";
    for (const std::filesystem::path& candidate : candidates) {
        if (!isFile(candidate)) {
            continue;
        }
        if (asset.locator.fingerprint.isKnown()) {
            const auto fingerprint = fingerprintFile(candidate);
            if (!fingerprint || !fingerprint->sameContentAs(asset.locator.fingerprint)) {
                reason = "a different file is at " + pathToUtf8(candidate);
                continue;
            }
        }
        out.locator.path = pathToUtf8(absolutePath(candidate));
        out.locator.offline = false;
        return {std::move(out), {}};
    }
    out.locator.offline = true;
    return {std::move(out), std::move(reason)};
}

ProjectState withMediaChecked(const ProjectState& state, const std::filesystem::path& projectFile, int* changedCount) {
    ProjectState out = state;
    int changed = 0;
    for (MediaPtr& media : out.media) {
        auto [checked, reason] = checkMediaOnDisk(*media, projectFile);
        if (!(checked.locator == media->locator)) {
            media = std::make_shared<MediaItem>(std::move(checked));
            ++changed;
        }
    }
    for (ProjectAsset& asset : out.assets) {
        auto [checked, reason] = checkAssetOnDisk(asset, projectFile);
        if (!(checked.locator == asset.locator)) {
            asset = std::move(checked);
            ++changed;
        }
    }
    if (changedCount) {
        *changedCount = changed;
    }
    return out;
}

// ---- ProjectDocument ----------------------------------------------------------------------------------

ProjectDocument::ProjectDocument(EditSession& session, ProjectDocumentOptions options)
    : m_session(session), m_options(std::move(options)), m_untitledId(newUntitledId()) {
    rememberClean();
}

ProjectDocument::~ProjectDocument() = default;

void ProjectDocument::rememberClean() {
    m_clean = m_session.snapshot();
    m_autosaved = m_clean;
}

std::string ProjectDocument::displayName() const {
    return m_file.empty() ? std::string("Untitled") : pathToUtf8(m_file.stem());
}

bool ProjectDocument::isDirty() const {
    const ProjectSnapshot now = m_session.snapshot();
    return !m_clean || !now || !isSameRevision(*m_clean, *now);
}

void ProjectDocument::markClean() {
    rememberClean();
}

void ProjectDocument::newProject(ProjectState initial) {
    m_session.reset(std::move(initial));
    m_file.clear();
    m_untitledId = newUntitledId();
    rememberClean();
}

std::expected<void, Error> ProjectDocument::open(const std::filesystem::path& file) {
    const std::filesystem::path absolute = absolutePath(file);
    auto loaded = io::loadProject(absolute);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    m_session.reset(withMediaChecked(*loaded, absolute));
    m_file = absolute;
    rememberClean();
    return {};
}

std::expected<void, Error> ProjectDocument::save() {
    if (m_file.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "the project has no file yet: use Save As"});
    }
    return saveAs(m_file);
}

std::expected<void, Error> ProjectDocument::saveAs(const std::filesystem::path& file) {
    const std::filesystem::path absolute = absolutePath(file);
    const ProjectSnapshot saved = m_session.snapshot();
    ProjectState state = rebased(m_session.stateForSave(), absolute);
    if (m_saveFilter) {
        m_saveFilter(state);
    }
    if (auto written = io::saveProject(state, absolute, io::SaveOptions{m_options.saveBackups}); !written) {
        return std::unexpected(std::move(written.error()));
    }
    m_file = absolute;
    m_clean = saved;
    m_autosaved = saved;
    return {};
}

// ---- Autosave -----------------------------------------------------------------------------------------

std::filesystem::path ProjectDocument::autosaveFile() const {
    if (!m_file.empty()) {
        return io::autosavePath(m_file);
    }
    if (m_options.untitledAutosaveDir.empty()) {
        return {};
    }
    return io::autosavePath(m_options.untitledAutosaveDir / ("Untitled-" + m_untitledId + std::string(io::kProjectExtension)));
}

std::expected<bool, Error> ProjectDocument::autosave() {
    const std::optional<AutosaveJob> job = prepareAutosave();
    if (!job) {
        return false;
    }
    if (auto written = job->write(); !written) {
        return std::unexpected(std::move(written.error()));
    }
    finishAutosave(*job);
    return true;
}

std::optional<ProjectDocument::AutosaveJob> ProjectDocument::prepareAutosave() const {
    AutosaveJob job;
    job.target = autosaveFile();
    if (job.target.empty()) {
        return std::nullopt;
    }
    job.snapshot = m_session.snapshot();
    if (m_autosaved && job.snapshot && isSameRevision(*m_autosaved, *job.snapshot)) {
        return std::nullopt;
    }
    job.createFolder = !m_file.empty() || !m_options.untitledAutosaveDir.empty();
    job.state = m_file.empty() ? m_session.stateForSave() : rebased(m_session.stateForSave(), m_file);
    if (m_saveFilter) {
        m_saveFilter(job.state);
    }
    job.copies = std::max(1, m_options.autosaveCopies);
    job.retire = m_retire;
    return job;
}

void ProjectDocument::finishAutosave(const AutosaveJob& job) {
    m_autosaved = job.snapshot;
    if (!job.retire.empty() && m_retire == job.retire) {
        m_retire.clear(); // its revision is safe in this session's autosave now (write() deleted it)
    }
}

std::expected<void, Error> ProjectDocument::AutosaveJob::write() const {
    // "<project>" whose autosave is `target` (io::autosavePath appends ".autosave").
    std::filesystem::path projectFile = target;
    projectFile.replace_extension();
    std::error_code ec;
    if (createFolder) {
        std::filesystem::create_directories(projectFile.parent_path(), ec);
    }
    // The copy being replaced, kept in memory: the new one is written first (atomically over it), and only then do
    // the older copies rotate. A failed or interrupted write leaves every existing copy as it was.
    const std::optional<std::string> previous = copies > 1 && isFile(target) ? readAll(target) : std::nullopt;
    const auto previousTime = previous ? std::filesystem::last_write_time(target, ec) : std::filesystem::file_time_type{};
    if (auto written = io::writeAutosave(state, projectFile); !written) {
        return std::unexpected(std::move(written.error()));
    }
    if (previous) {
        // autosave<N-1> -> autosave<N>, ..., autosave2 -> autosave3; the replaced copy becomes autosave2.
        for (int n = copies - 1; n >= 2; --n) {
            const std::filesystem::path from = rotatedAutosave(projectFile, n);
            if (isFile(from)) {
                std::filesystem::rename(from, rotatedAutosave(projectFile, n + 1), ec);
            }
        }
        if (io::writeFileAtomically(rotatedAutosave(projectFile, 2), *previous)) {
            std::filesystem::last_write_time(rotatedAutosave(projectFile, 2), previousTime, ec); // when it was saved
        }
    }
    if (!retire.empty()) {
        // An untitled session recovered from: its revision is safe in this session's autosave now.
        if (const auto parsed = untitledAutosaveName(pathToUtf8(retire.filename()))) {
            const std::filesystem::path session = retire.parent_path() / pathFromUtf8(parsed->first);
            for (int n = parsed->second; n <= std::max(copies, parsed->second); ++n) {
                if (const std::filesystem::path copy = rotatedAutosave(session, n); copy != target) {
                    std::filesystem::remove(copy, ec);
                }
            }
        }
    }
    return {};
}

std::vector<RecoveryFile> ProjectDocument::recoveryFiles(const std::filesystem::path& projectFile) const {
    std::vector<RecoveryFile> out;
    for (const io::RecoveryCandidate& c : io::recoveryCandidates(projectFile)) {
        out.push_back({c.file,
                       c.kind == io::RecoveryCandidate::Kind::Autosave ? RecoveryFile::Kind::Autosave
                                                                       : RecoveryFile::Kind::Backup,
                       c.modified, c.newerThanProject, c.loadable});
    }
    std::error_code ec;
    const bool projectExists = isFile(projectFile);
    const auto projectTime = projectExists ? std::filesystem::last_write_time(projectFile, ec)
                                           : std::filesystem::file_time_type::min();
    for (int n = 2; n <= std::max(1, m_options.autosaveCopies); ++n) {
        const std::filesystem::path file = rotatedAutosave(projectFile, n);
        if (!isFile(file)) {
            continue;
        }
        RecoveryFile r;
        r.file = file;
        r.kind = RecoveryFile::Kind::Autosave;
        r.modified = std::filesystem::last_write_time(file, ec);
        r.newerThanProject = !projectExists || r.modified > projectTime;
        r.loadable = io::loadProject(file).has_value();
        out.push_back(std::move(r));
    }
    std::ranges::sort(out, [](const RecoveryFile& a, const RecoveryFile& b) { return a.modified > b.modified; });
    return out;
}

std::expected<void, Error> ProjectDocument::recover(const std::filesystem::path& recoveryFile,
                                                    const std::filesystem::path& projectFile) {
    auto loaded = io::loadProject(recoveryFile);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    const std::filesystem::path absolute = projectFile.empty() ? std::filesystem::path{} : absolutePath(projectFile);
    m_retire.clear();
    if (absolute.empty()) {
        m_untitledId = newUntitledId(); // a new session of its own; the recovered file stays until replaced
        if (untitledAutosaveName(pathToUtf8(recoveryFile.filename()))) {
            m_retire = absolutePath(recoveryFile);
        }
    }
    // Relative media resolve against the project the revision belongs to, also when it opens as an untitled copy.
    const std::filesystem::path base = !absolute.empty() ? absolute : projectOfRecoveryFile(absolutePath(recoveryFile));
    m_session.reset(withMediaChecked(*loaded, base));
    m_file = absolute;
    m_clean.reset();    // recovered work is not on disk as the project yet
    m_autosaved.reset(); // and the next autosave() writes it, whatever was autosaved before
    return {};
}

std::filesystem::path ProjectDocument::projectOfRecoveryFile(const std::filesystem::path& recoveryFile) {
    std::string name = pathToUtf8(recoveryFile.filename());
    const std::string extension(io::kProjectExtension);
    for (const char* suffix : {".autosave", ".bak"}) {
        const std::size_t at = name.rfind(extension + suffix);
        if (at == std::string::npos) {
            continue;
        }
        const std::string number = name.substr(at + extension.size() + std::char_traits<char>::length(suffix));
        if (std::ranges::all_of(number, [](unsigned char c) { return std::isdigit(c) != 0; }) &&
            (std::string_view(suffix) == ".autosave" || !number.empty())) {
            if (name.starts_with("Untitled-") && std::string_view(suffix) == ".autosave") {
                return {}; // an untitled session's autosave: no project
            }
            return recoveryFile.parent_path() / pathFromUtf8(name.substr(0, at + extension.size()));
        }
    }
    return name.ends_with(extension) ? recoveryFile : std::filesystem::path{};
}

std::optional<RecoveryFile> ProjectDocument::newestUnsavedAutosave(const std::filesystem::path& projectFile,
                                                                   int autosaveCopies) {
    std::error_code ec;
    const bool projectExists = isFile(projectFile);
    const auto projectTime = projectExists ? std::filesystem::last_write_time(projectFile, ec)
                                           : std::filesystem::file_time_type::min();
    std::vector<RecoveryFile> candidates;
    for (int n = 1; n <= std::max(1, autosaveCopies); ++n) {
        const std::filesystem::path file = rotatedAutosave(projectFile, n);
        if (!isFile(file)) {
            continue;
        }
        RecoveryFile r;
        r.file = file;
        r.kind = RecoveryFile::Kind::Autosave;
        r.modified = std::filesystem::last_write_time(file, ec);
        r.newerThanProject = !projectExists || r.modified > projectTime;
        if (r.newerThanProject) { // an older autosave is never offered: no need to read it
            candidates.push_back(std::move(r));
        }
    }
    std::ranges::stable_sort(candidates, [](const RecoveryFile& a, const RecoveryFile& b) { return a.modified > b.modified; });
    for (RecoveryFile& r : candidates) {
        if (io::loadProject(r.file).has_value()) {
            r.loadable = true;
            return std::move(r);
        }
    }
    return std::nullopt;
}

void ProjectDocument::removeAutosaves() {
    const std::filesystem::path newest = autosaveFile();
    if (newest.empty()) {
        return;
    }
    std::filesystem::path projectFile = newest;
    projectFile.replace_extension();
    std::error_code ec;
    for (int n = 1; n <= std::max(1, m_options.autosaveCopies); ++n) {
        std::filesystem::remove(rotatedAutosave(projectFile, n), ec);
    }
    m_autosaved.reset();
}

std::vector<RecoveryFile> ProjectDocument::untitledRecoveryFiles(const std::filesystem::path& untitledAutosaveDir) {
    // Every copy of every untitled session: "Untitled-<id>.wabcut.autosave" and its rotated "...autosave<n>". A crash
    // can leave a session with numbered copies only, or with a newest copy that does not load.
    struct Copy {
        std::filesystem::path file;
        int n = 1;
        std::filesystem::file_time_type modified;
    };
    std::map<std::string, std::vector<Copy>> sessions;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(untitledAutosaveDir, ec)) {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc)) {
            continue;
        }
        if (const auto parsed = untitledAutosaveName(pathToUtf8(entry.path().filename()))) {
            sessions[parsed->first].push_back({entry.path(), parsed->second, std::filesystem::last_write_time(entry.path(), fileEc)});
        }
    }
    std::vector<RecoveryFile> out;
    for (auto& [session, copies] : sessions) {
        // Newest first: the rotation number says it (1 = the newest), the time breaks ties.
        std::ranges::sort(copies, [](const Copy& a, const Copy& b) { return a.n != b.n ? a.n < b.n : a.modified > b.modified; });
        for (const Copy& c : copies) {
            if (io::loadProject(c.file).has_value()) { // the latest copy of the session that loads
                out.push_back({c.file, RecoveryFile::Kind::Autosave, c.modified, true, true});
                break;
            }
        }
    }
    std::ranges::sort(out, [](const RecoveryFile& a, const RecoveryFile& b) { return a.modified > b.modified; });
    return out;
}

// ---- Offline media and relinking ------------------------------------------------------------------------

std::vector<MediaStatus> ProjectDocument::mediaStatus() const {
    std::vector<MediaStatus> out;
    const ProjectSnapshot snap = m_session.snapshot();
    for (const MediaPtr& media : snap->media) {
        out.push_back({media->id, media->name, media->locator.path, media->locator.offline,
                       media->locator.offline ? std::string("not found") : std::string{}});
    }
    return out;
}

std::vector<MediaId> ProjectDocument::offlineMedia() const {
    std::vector<MediaId> out;
    for (const MediaPtr& media : m_session.snapshot()->media) {
        if (media->locator.offline) {
            out.push_back(media->id);
        }
    }
    return out;
}

std::expected<int, Error> ProjectDocument::refreshMediaStatus() {
    int changed = 0;
    ProjectState next = withMediaChecked(*m_session.snapshot(), m_file, &changed);
    if (changed == 0) {
        return 0;
    }
    if (auto committed = m_session.commit("Refresh Media", next); !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return changed;
}

std::vector<RelinkCandidate> ProjectDocument::findRelinkCandidates(
    MediaId mediaId, const std::vector<std::filesystem::path>& folders) const {
    std::vector<RelinkCandidate> out;
    const ProjectSnapshot snap = m_session.snapshot();
    const MediaItem* media = snap->findMedia(mediaId);
    if (!media || media->isGenerator()) {
        return out;
    }
    // The file name after the last separator of either kind: a project saved on Windows names "C:\\Media\\a.mov",
    // whose filename() on macOS/Linux would be the whole string (I.7 portability audit).
    const std::string& recorded = media->locator.path.empty() ? media->name : media->locator.path;
    const std::size_t separator = recorded.find_last_of("/\\");
    const std::string wanted = lower(separator == std::string::npos ? recorded : recorded.substr(separator + 1));
    std::size_t visited = 0;
    for (const std::filesystem::path& folder : folders) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            folder, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (++visited > kMaxSearchEntries) {
                break;
            }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || lower(pathToUtf8(it->path().filename())) != wanted) {
                continue;
            }
            const std::filesystem::path found = absolutePath(it->path());
            if (std::ranges::any_of(out, [&](const RelinkCandidate& c) { return c.file == found; })) {
                continue;
            }
            const auto fingerprint = fingerprintFile(found);
            out.push_back({found, fingerprint && fingerprint->sameContentAs(media->locator.fingerprint)});
        }
    }
    std::ranges::stable_sort(out, [](const RelinkCandidate& a, const RelinkCandidate& b) {
        return a.fingerprintMatches && !b.fingerprintMatches;
    });
    return out;
}

std::expected<void, Error> ProjectDocument::relink(MediaId mediaId, const std::filesystem::path& file, bool force) {
    const ProjectSnapshot snap = m_session.snapshot();
    const MediaItem* media = snap->findMedia(mediaId);
    if (!media) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such media item"});
    }
    if (media->isGenerator()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, media->name + " is generated (a title): it has no file"});
    }
    auto described = describeMediaFile(file, m_file);
    if (!described) {
        return std::unexpected(std::move(described.error()));
    }
    if (!force && media->locator.fingerprint.isKnown() &&
        !described->locator.fingerprint.sameContentAs(media->locator.fingerprint)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     pathToUtf8(file.filename()) + " is not the same file as " + media->name});
    }
    auto committed = m_session.apply("Relink Media", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        for (MediaPtr& m : next.media) {
            if (m->id == mediaId) {
                auto item = std::make_shared<MediaItem>(*m);
                item->locator.path = described->locator.path;
                item->locator.relativePath = described->locator.relativePath;
                item->locator.fingerprint = described->locator.fingerprint;
                item->locator.offline = false;
                item->info = described->info;
                m = std::move(item);
            }
        }
        return next;
    });
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return {};
}

std::expected<std::vector<MediaId>, Error> ProjectDocument::relinkAll(
    const std::vector<std::filesystem::path>& folders) {
    std::vector<std::pair<MediaId, std::filesystem::path>> found;
    for (MediaId id : offlineMedia()) {
        for (const RelinkCandidate& c : findRelinkCandidates(id, folders)) {
            if (c.fingerprintMatches) {
                found.emplace_back(id, c.file);
                break;
            }
        }
    }
    std::vector<std::pair<AssetId, std::filesystem::path>> foundAssets;
    for (const AssetStatus& a : assetStatus()) {
        if (!a.offline) {
            continue;
        }
        for (const RelinkCandidate& c : findAssetRelinkCandidates(a.asset, folders)) {
            if (c.fingerprintMatches) {
                foundAssets.emplace_back(a.asset, c.file);
                break;
            }
        }
    }
    std::vector<MediaId> relinked;
    if (found.empty() && foundAssets.empty()) {
        return relinked;
    }
    auto committed = m_session.apply("Relink Media", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        relinked.clear();
        for (MediaPtr& m : next.media) {
            const auto hit = std::ranges::find_if(found, [&](const auto& f) { return f.first == m->id; });
            if (hit == found.end()) {
                continue;
            }
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.path = pathToUtf8(hit->second);
            item->locator.relativePath = m_file.empty() ? std::string{} : relativeMediaPath(m_file, hit->second);
            item->locator.offline = false;
            m = std::move(item);
            relinked.push_back(m->id);
        }
        for (const auto& [asset, file] : foundAssets) {
            MediaLocator locator = next.findAsset(asset)->locator;
            locator.path = pathToUtf8(file);
            locator.relativePath = m_file.empty() ? std::string{} : relativeMediaPath(m_file, file);
            locator.offline = false;
            auto moved = edit::relinkAsset(next, asset, std::move(locator));
            if (!moved) {
                return std::unexpected(moved.error());
            }
            next = std::move(*moved);
        }
        return next;
    });
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return relinked;
}

std::vector<AssetStatus> ProjectDocument::assetStatus() const {
    std::vector<AssetStatus> out;
    for (const ProjectAsset& a : m_session.snapshot()->assets) {
        out.push_back({a.id, a.kind, a.name, a.locator.path, a.locator.offline});
    }
    return out;
}

std::vector<RelinkCandidate> ProjectDocument::findAssetRelinkCandidates(AssetId assetId,
                                                                       const std::vector<std::filesystem::path>& folders) const {
    std::vector<RelinkCandidate> out;
    const ProjectSnapshot snap = m_session.snapshot();
    const ProjectAsset* asset = snap->findAsset(assetId);
    if (!asset) {
        return out;
    }
    const std::string wanted = lower(pathToUtf8(pathFromUtf8(asset->locator.path.empty() ? asset->name : asset->locator.path).filename()));
    std::size_t visited = 0;
    for (const std::filesystem::path& folder : folders) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (++visited > kMaxSearchEntries) {
                break;
            }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || lower(pathToUtf8(it->path().filename())) != wanted) {
                continue;
            }
            const std::filesystem::path found = absolutePath(it->path());
            if (std::ranges::any_of(out, [&](const RelinkCandidate& c) { return c.file == found; })) {
                continue;
            }
            const auto fingerprint = fingerprintFile(found);
            out.push_back({found, fingerprint && fingerprint->sameContentAs(asset->locator.fingerprint)});
        }
    }
    std::ranges::stable_sort(out, [](const RelinkCandidate& a, const RelinkCandidate& b) { return a.fingerprintMatches && !b.fingerprintMatches; });
    return out;
}

std::expected<void, Error> ProjectDocument::relinkAsset(AssetId assetId, const std::filesystem::path& file, bool force) {
    const ProjectSnapshot snap = m_session.snapshot();
    const ProjectAsset* asset = snap->findAsset(assetId);
    if (!asset) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such LUT"});
    }
    if (!isFile(file)) {
        return std::unexpected(Error{ErrorCode::NotFound, pathToUtf8(file) + " does not exist"});
    }
    const auto fingerprint = fingerprintFile(file);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    if (!force && asset->locator.fingerprint.isKnown() && !fingerprint->sameContentAs(asset->locator.fingerprint)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, pathToUtf8(file.filename()) + " is not the same file as " + asset->name});
    }
    MediaLocator locator = asset->locator;
    locator.path = pathToUtf8(absolutePath(file));
    locator.relativePath = m_file.empty() ? std::string{} : relativeMediaPath(m_file, absolutePath(file));
    locator.fingerprint = *fingerprint;
    locator.offline = false;
    auto committed = m_session.apply("Relink LUT", [&](const ProjectState& state) { return edit::relinkAsset(state, assetId, locator); });
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return {};
}

} // namespace wabcut::editor
