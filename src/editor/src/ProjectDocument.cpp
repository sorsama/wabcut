#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>

#include <wabcut/model/Auditions.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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
    if (item.madeOffline) { // P.2: Make Offline keeps it offline until it is linked again
        out.locator.offline = true;
        return {std::move(out), "made offline"};
    }
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
        if (!mediaFileExists(candidate)) { // an image sequence: its first frame (ImageSequence.h)
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
    m_clean = m_session.committed(); // never a preview (an audition's take playing): it is not saved
    m_autosaved = m_clean;
}

std::string ProjectDocument::displayName() const {
    return m_file.empty() ? std::string("Untitled") : pathToUtf8(m_file.stem());
}

bool ProjectDocument::isDirty() const {
    const ProjectSnapshot now = m_session.committed();
    return !m_clean || !now || !isSameRevision(*m_clean, *now);
}

void ProjectDocument::markClean() {
    rememberClean();
}

ProjectDocument::Identity ProjectDocument::exchangeIdentity(Identity other) {
    Identity out{std::move(m_file), std::move(m_clean), std::move(m_autosaved), std::move(m_untitledId), std::move(m_retire)};
    m_file = std::move(other.file);
    m_clean = std::move(other.clean);
    m_autosaved = std::move(other.autosaved);
    m_untitledId = other.untitledId.empty() ? newUntitledId() : std::move(other.untitledId);
    m_retire = std::move(other.retire);
    return out;
}

bool ProjectDocument::isDirty(const Identity& identity, const ProjectSnapshot& state) {
    return !identity.clean || !state || !isSameRevision(*identity.clean, *state);
}

void ProjectDocument::removeAutosavesOf(const Identity& identity) const {
    const std::filesystem::path newest = autosaveFileFor(identity.file, identity.untitledId);
    if (newest.empty()) {
        return;
    }
    std::filesystem::path projectFile = newest;
    projectFile.replace_extension();
    std::error_code ec;
    for (int n = 1; n <= std::max(1, m_options.autosaveCopies); ++n) {
        std::filesystem::remove(rotatedAutosave(projectFile, n), ec);
    }
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
    const ProjectSnapshot saved = m_session.committed();
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

std::expected<void, Error> ProjectDocument::saveCopy(const std::filesystem::path& file) const {
    const std::filesystem::path absolute = absolutePath(file);
    ProjectState state = rebased(m_session.stateForSave(), absolute);
    if (m_saveFilter) {
        m_saveFilter(state);
    }
    // A copy is a one-off file (the user confirmed replacing one that was there): no ".bakN" rotation next to it.
    return io::saveProject(state, absolute, io::SaveOptions{0});
}

// ---- Autosave -----------------------------------------------------------------------------------------

std::filesystem::path ProjectDocument::autosaveFile() const {
    return autosaveFileFor(m_file, m_untitledId);
}

std::filesystem::path ProjectDocument::autosaveFileFor(const std::filesystem::path& file, const std::string& untitledId) const {
    if (!file.empty()) {
        return io::autosavePath(file);
    }
    if (m_options.untitledAutosaveDir.empty() || untitledId.empty()) {
        return {};
    }
    return io::autosavePath(m_options.untitledAutosaveDir / ("Untitled-" + untitledId + std::string(io::kProjectExtension)));
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
    job.snapshot = m_session.committed();
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

namespace {

/// The file name (lower case) an item's file had: after the last separator of either kind (a project saved on Windows
/// names "C:\\Media\\a.mov", whose filename() on macOS/Linux would be the whole string, I.7 portability audit), of the
/// file that stands for an image sequence, spanned clip or layered-file item (0.2: its first frame, part, PSD).
std::string recordedFileName(const MediaItem& media) {
    const std::string recorded = media.locator.path.empty() ? media.name
                                                            : pathToUtf8(mediaFileToCheck(pathFromUtf8(media.locator.path)));
    const std::size_t separator = recorded.find_last_of("/\\");
    return lower(separator == std::string::npos ? recorded : recorded.substr(separator + 1));
}

/// "clip.mkv" -> {"clip", ".mkv"} (lower case already).
std::pair<std::string, std::string> stemAndExtension(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return {name, {}};
    }
    return {name.substr(0, dot), name.substr(dot)};
}

/// Calls `fn(lowerCaseName, absolutePath)` for every regular file under `folders` (recursive, at most kMaxSearchEntries
/// entries in all).
template <typename Fn>
void forEachFile(const std::vector<std::filesystem::path>& folders, Fn&& fn) {
    std::size_t visited = 0;
    for (const std::filesystem::path& folder : folders) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied,
                                                         ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (++visited > kMaxSearchEntries) {
                break;
            }
            std::error_code fileEc;
            if (it->is_regular_file(fileEc)) {
                fn(lower(pathToUtf8(it->path().filename())), it->path());
            }
        }
    }
}

/// Every regular file under `folders` by lower-case file name (forEachFile).
std::multimap<std::string, std::filesystem::path> filesByName(const std::vector<std::filesystem::path>& folders) {
    std::multimap<std::string, std::filesystem::path> out;
    forEachFile(folders, [&](const std::string& name, const std::filesystem::path& file) {
        out.emplace(name, absolutePath(file));
    });
    return out;
}

/// A plain file's item (not an image sequence, spanned clip or layered file, whose path says more than a file).
bool plainFileItem(const MediaItem& media) {
    return !media.isGenerator() && !media.locator.path.empty() &&
           mediaFileToCheck(pathFromUtf8(media.locator.path)) == pathFromUtf8(media.locator.path);
}

/// P1 round (#42): `probed` is `media`'s recording in another container: the same length (within a frame, or 50 ms -
/// an audio packet - whichever is longer: a remux may end its audio a packet earlier), picture size and frame rate,
/// audio streams and channels.
bool sameRecordingInfo(const MediaInfo& original, const MediaInfo& probed) {
    if (original.isStillImage || probed.isStillImage || original.duration.value <= 0 || probed.duration.value <= 0) {
        return false;
    }
    double frame = 1.0 / 30.0;
    if (original.hasVideo() && original.video.front().frameRate.isValid()) {
        frame = 1.0 / original.video.front().frameRate.toDouble();
    }
    if (std::abs(original.duration.toSeconds() - probed.duration.toSeconds()) > std::max(frame, 0.05) + 1e-6) {
        return false;
    }
    if (original.video.size() != probed.video.size() || original.audio.size() != probed.audio.size()) {
        return false;
    }
    if (original.hasVideo()) {
        const VideoStreamInfo& a = original.video.front();
        const VideoStreamInfo& b = probed.video.front();
        if (a.width != b.width || a.height != b.height) {
            return false;
        }
        if (a.frameRate.isValid() != b.frameRate.isValid() ||
            (a.frameRate.isValid() && std::abs(a.frameRate.toDouble() - b.frameRate.toDouble()) > 0.01)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < original.audio.size(); ++i) {
        if (original.audio[i].channels != probed.audio[i].channels) {
            return false;
        }
    }
    return true;
}

/// Why `next` cannot have `item` (or its subclips) play `info`: a clip of a sequence plays a stream it lacks; nullopt
/// when every clip finds its stream.
std::optional<std::string> missingStreamProblem(const ProjectState& state, const MediaItem& item, const MediaInfo& info) {
    MediaItem probe = item;
    probe.info = info;
    std::optional<std::string> problem;
    for (const SequencePtr& s : state.sequences) {
        const auto check = [&](MediaId media, StreamRef stream, const std::string& what) {
            if (problem || !media.isValid()) {
                return;
            }
            const MediaItem* used = state.findMedia(media);
            if (!used || (used->id != item.id && !(used->subclip && used->subclip->master == item.id))) {
                return;
            }
            if (!probe.hasStream(stream)) {
                problem = "the new file has no " + std::string(stream.kind == StreamKind::Video ? "video" : "audio") +
                          " stream " + std::to_string(stream.index + 1) + " for " + what + " in '" + s->name + "'";
            }
        };
        s->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                check(c.media, c.stream, "clip '" + c.name() + "'");
                if (const Audition* a = c.audition()) { // 0.6 (card #74): an audition's other takes play it too
                    for (const AuditionTake& take : a->others) {
                        check(take.media, take.stream, "take '" + take.name() + "' of clip '" + c.name() + "'");
                    }
                }
            }
        });
        if (problem) {
            break;
        }
    }
    return problem;
}

} // namespace

std::vector<RelinkCandidate> ProjectDocument::findRelinkCandidates(MediaId mediaId,
                                                                  const std::vector<std::filesystem::path>& folders,
                                                                  bool matchStem) const {
    std::vector<RelinkCandidate> out;
    const ProjectSnapshot snap = m_session.snapshot();
    const MediaItem* media = snap->findMedia(mediaId);
    if (!media || media->isGenerator()) {
        return out;
    }
    const std::string wanted = recordedFileName(*media);
    const std::string wantedStem = stemAndExtension(wanted).first;
    const bool byStem = matchStem && plainFileItem(*media);
    std::vector<std::filesystem::path> stemMatches;
    forEachFile(folders, [&](const std::string& name, const std::filesystem::path& path) {
        const bool named = name == wanted;
        if (!named && !(byStem && stemAndExtension(name).first == wantedStem)) {
            return;
        }
        const std::filesystem::path found = absolutePath(path);
        if (std::ranges::any_of(out, [&](const RelinkCandidate& c) { return c.file == found; }) ||
            std::ranges::find(stemMatches, found) != stemMatches.end()) {
            return;
        }
        if (named) {
            const auto fingerprint = fingerprintFile(found);
            out.push_back({found, fingerprint && fingerprint->sameContentAs(media->locator.fingerprint)});
        } else {
            stemMatches.push_back(found);
        }
    });
    std::ranges::stable_sort(out, [](const RelinkCandidate& a, const RelinkCandidate& b) {
        return a.fingerprintMatches && !b.fingerprintMatches;
    });
    for (const std::filesystem::path& found : stemMatches) { // P1 round (#42): another container, after the name matches
        out.push_back({found, false, isSameRecording(mediaId, found)});
    }
    return out;
}

bool ProjectDocument::isSameRecording(MediaId mediaId, const std::filesystem::path& file) const {
    const ProjectSnapshot snap = m_session.snapshot();
    const MediaItem* media = snap->findMedia(mediaId);
    if (!media || !plainFileItem(*media) || !isFile(file)) {
        return false;
    }
    const auto described = describeMediaFile(file, m_file);
    return described && sameRecordingInfo(media->info, described->info);
}

std::expected<int, Error> ProjectDocument::relinkAfterLocate(MediaId mediaId, const std::filesystem::path& file,
                                                             bool force) {
    const ProjectSnapshot snap = m_session.snapshot();
    const MediaItem* media = snap->findMedia(mediaId);
    if (!media) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such media item"});
    }
    if (media->isGenerator()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, media->name + " is generated (a title): it has no file"});
    }
    auto described = describeMediaFile(pathFromUtf8(relinkedMediaPath(media->locator.path, file)), m_file);
    if (!described) {
        return std::unexpected(std::move(described.error()));
    }
    const bool sameContent =
        !media->locator.fingerprint.isKnown() || described->locator.fingerprint.sameContentAs(media->locator.fingerprint);
    if (!sameContent && !force) {
        if (plainFileItem(*media) && sameRecordingInfo(media->info, described->info)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         pathToUtf8(file.filename()) + " looks like the same recording as " + media->name +
                                             " in another container, but it is not the same file"});
        }
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     pathToUtf8(file.filename()) + " is not the same file as " + media->name});
    }
    if (!sameContent) { // forced: every clip must still find its stream
        if (const auto problem = missingStreamProblem(*snap, *media, described->info)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, *problem});
        }
    }

    // The others: the same place under the moved folder, then anything named like them under the located file's folder.
    std::vector<std::pair<MediaId, std::filesystem::path>> found;
    std::vector<const MediaItem*> rest;
    for (const MediaPtr& m : snap->media) {
        if (m->id != mediaId && m->locator.offline && !m->isGenerator() && !m->locator.path.empty()) {
            rest.push_back(m.get());
        }
    }
    const auto take = [&](const MediaItem& item, const std::filesystem::path& candidate) {
        if (!mediaFileExists(candidate)) {
            return false;
        }
        const auto fingerprint = fingerprintFile(mediaFileToCheck(candidate));
        if (!fingerprint || !fingerprint->sameContentAs(item.locator.fingerprint)) {
            return false;
        }
        found.emplace_back(item.id, candidate);
        return true;
    };
    const std::filesystem::path oldFile = absolutePath(mediaFileToCheck(pathFromUtf8(media->locator.path)));
    const std::filesystem::path newFile = absolutePath(file);
    std::filesystem::path oldDir = oldFile.parent_path();
    std::filesystem::path newDir = newFile.parent_path();
    for (int level = 0; level <= 3 && !rest.empty() && !oldDir.empty() && !newDir.empty(); ++level) {
        if (level > 0) {
            // One folder up on both sides, while the folders left behind are named alike (the same tree, moved).
            if (lower(pathToUtf8(oldDir.filename())) != lower(pathToUtf8(newDir.filename())) ||
                oldDir.parent_path() == oldDir || newDir.parent_path() == newDir) {
                break;
            }
            oldDir = oldDir.parent_path();
            newDir = newDir.parent_path();
        }
        std::erase_if(rest, [&](const MediaItem* item) {
            const std::filesystem::path was = absolutePath(mediaFileToCheck(pathFromUtf8(item->locator.path)));
            const std::filesystem::path relative = was.lexically_relative(oldDir);
            if (relative.empty() || relative.begin()->string() == "..") {
                return false;
            }
            return take(*item, (newDir / relative).lexically_normal());
        });
    }
    if (!rest.empty()) {
        const auto files = filesByName({newFile.parent_path()});
        std::erase_if(rest, [&](const MediaItem* item) {
            const auto [first, last] = files.equal_range(recordedFileName(*item));
            for (auto it = first; it != last; ++it) {
                if (take(*item, it->second)) {
                    return true;
                }
            }
            return false;
        });
    }

    int others = 0;
    auto committed = m_session.apply("Relink Media", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        others = 0;
        for (MediaPtr& m : next.media) {
            if (m->id == mediaId) {
                auto item = std::make_shared<MediaItem>(*m);
                item->locator.path = described->locator.path;
                item->locator.relativePath = described->locator.relativePath;
                item->locator.fingerprint = described->locator.fingerprint;
                item->locator.offline = false;
                item->madeOffline = false;
                item->info = described->info;
                m = std::move(item);
                continue;
            }
            const auto hit = std::ranges::find_if(found, [&](const auto& f) { return f.first == m->id; });
            if (hit == found.end()) {
                continue;
            }
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.path = relinkedMediaPath(m->locator.path, mediaFileToCheck(hit->second));
            item->locator.relativePath =
                m_file.empty() ? std::string{} : relativeMediaPath(m_file, pathFromUtf8(item->locator.path));
            item->locator.offline = false;
            item->madeOffline = false;
            m = std::move(item);
            ++others;
        }
        return next;
    });
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return others;
}

std::vector<SameRecording> ProjectDocument::findSameRecordings(const std::vector<std::filesystem::path>& folders) const {
    std::vector<SameRecording> out;
    const ProjectSnapshot snap = m_session.snapshot();
    std::vector<const MediaItem*> offline;
    for (const MediaPtr& m : snap->media) {
        // Subclips follow their master (relinkSameRecordings moves them with it).
        if (m->locator.offline && plainFileItem(*m) && !m->isSubclip()) {
            offline.push_back(m.get());
        }
    }
    if (offline.empty()) {
        return out;
    }
    std::multimap<std::string, std::filesystem::path> byStem;
    for (const auto& [name, file] : filesByName(folders)) {
        byStem.emplace(stemAndExtension(name).first, file);
    }
    std::map<std::filesystem::path, std::optional<MediaInfo>> probed; // each file is read once
    for (const MediaItem* item : offline) {
        const auto [stem, extension] = stemAndExtension(recordedFileName(*item));
        const auto [first, last] = byStem.equal_range(stem);
        for (auto it = first; it != last; ++it) {
            if (stemAndExtension(lower(pathToUtf8(it->second.filename()))).second == extension) {
                continue; // the same name: a plain relink's business
            }
            auto [slot, inserted] = probed.try_emplace(it->second);
            if (inserted) {
                if (auto described = describeMediaFile(it->second, m_file)) {
                    slot->second = described->info;
                }
            }
            if (slot->second && sameRecordingInfo(item->info, *slot->second)) {
                out.push_back({item->id, it->second});
                break;
            }
        }
    }
    return out;
}

std::expected<int, Error> ProjectDocument::relinkSameRecordings(const std::vector<SameRecording>& offers) {
    const ProjectSnapshot snap = m_session.snapshot();
    struct Planned {
        MediaId media;
        std::string oldPath;
        MediaItem described;
    };
    std::vector<Planned> planned;
    for (const SameRecording& offer : offers) {
        const MediaItem* media = snap->findMedia(offer.media);
        if (!media) {
            return std::unexpected(Error{ErrorCode::NotFound, "no such media item"});
        }
        if (!plainFileItem(*media)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, media->name + " has no plain file to relink"});
        }
        auto described = describeMediaFile(offer.file, m_file);
        if (!described) {
            return std::unexpected(std::move(described.error()));
        }
        if (!sameRecordingInfo(media->info, described->info)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, pathToUtf8(offer.file.filename()) +
                                                                         " is not the same recording as " + media->name});
        }
        if (const auto problem = missingStreamProblem(*snap, *media, described->info)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, *problem});
        }
        planned.push_back({media->id, media->locator.path, std::move(*described)});
    }
    if (planned.empty()) {
        return 0;
    }
    auto committed = m_session.apply("Relink Media", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        for (MediaPtr& m : next.media) {
            const auto hit = std::ranges::find_if(planned, [&](const Planned& p) {
                return p.media == m->id ||
                       (m->subclip && m->subclip->master == p.media && m->locator.path == p.oldPath); // its subclips
            });
            if (hit == planned.end()) {
                continue;
            }
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.path = hit->described.locator.path;
            item->locator.relativePath = hit->described.locator.relativePath;
            item->locator.fingerprint = hit->described.locator.fingerprint;
            item->locator.offline = false;
            item->madeOffline = false;
            item->info = hit->described.info; // the new container's streams and indexes
            m = std::move(item);
        }
        return next;
    });
    if (!committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return static_cast<int>(planned.size());
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
    // 0.2: an image sequence, spanned clip or layered-file item keeps what its path says beyond the file.
    auto described = describeMediaFile(pathFromUtf8(relinkedMediaPath(media->locator.path, file)), m_file);
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
                item->madeOffline = false; // P.2: linked again
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
            item->locator.path = relinkedMediaPath(m->locator.path, hit->second); // 0.2: virtual paths keep their query
            item->locator.relativePath =
                m_file.empty() ? std::string{} : relativeMediaPath(m_file, pathFromUtf8(item->locator.path));
            item->locator.offline = false;
            item->madeOffline = false; // P.2: linked again
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
