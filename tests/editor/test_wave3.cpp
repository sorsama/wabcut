// Codex wave-3 regressions (docs/internal/reviews-codex-wave3.md) in the editor services: project-supplied proxy paths never
// delete what Wabcut did not make (P1-1), Recover keeps the recovered revision until a replacement is on disk (P1-3),
// Collect Files never lets names that differ only by case overwrite each other and a failed verification is an error
// (P1-4), every untitled autosave rotation/write interruption leaves a listed copy (P2-7), and Open Recovery Copy finds
// relative media of relocated copies (P2-11). Each case failed before its fix.

#include <wabcut/editor/CacheInspector.h>
#include <wabcut/editor/Capsule.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/ProxyManager.h>
#include <wabcut/editor/Recovery.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#endif

using namespace wabcut;
using namespace wabcut::editor;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / "wave3" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeText(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string readText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A directory junction (Windows; a directory symlink elsewhere). False when the platform refuses.
bool makeJunction(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
    const std::wstring command = L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >NUL 2>&1";
    return _wsystem(command.c_str()) == 0 && fs::exists(link);
#else
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
}

/// A video media item whose original is `original` (a real file, fingerprinted).
MediaItem videoItem(MediaId id, const fs::path& original) {
    MediaItem m;
    m.id = id;
    m.name = pathToUtf8(original.filename());
    m.locator.path = pathToUtf8(original);
    m.info.path = m.locator.path;
    m.info.video.push_back(VideoStreamInfo{});
    m.locator.fingerprint = fingerprintFile(original).value();
    return m;
}

ProxyProvenance provenanceOf(const MediaItem& m, std::uint64_t generation = 1) {
    ProxyProvenance p;
    p.generator = std::string(exporter::kProxyGenerator);
    p.settings = "some other settings";
    p.source = m.locator.fingerprint;
    p.generation = generation;
    return p;
}

/// "<folder>/proxies.json" with entries naming `files` (as written), each claiming to be `item`'s proxy.
void writeIndex(const fs::path& folder, const MediaItem& item, const std::vector<std::string>& files) {
    nlohmann::json list = nlohmann::json::array();
    std::uint64_t generation = 10;
    for (const std::string& f : files) {
        list.push_back({{"file", f},
                        {"sourcePath", item.locator.path},
                        {"generator", std::string(exporter::kProxyGenerator)},
                        {"settings", "x"},
                        {"generation", generation++},
                        {"source",
                         {{"size", item.locator.fingerprint.size},
                          {"mtimeNs", item.locator.fingerprint.mtimeNs},
                          {"sampleHash", item.locator.fingerprint.sampleHash}}}});
    }
    writeText(folder / "proxies.json",
              nlohmann::json{{"version", 1}, {"generator", std::string(exporter::kProxyGenerator)}, {"proxies", list}}.dump(2));
}

/// A project holding `items`, saved as `projectFile` (its managed proxy folder is "<stem> Proxies" next to it).
struct ProxyProject {
    EditSession session;
    std::unique_ptr<ProjectDocument> doc;
    std::unique_ptr<ProxyManager> manager;

    ProxyProject(const std::vector<MediaItem>& items, const fs::path& projectFile, ProxyQueue::Transcoder transcoder = {}) {
        ProjectState state;
        state.ids.next = 1000;
        for (const MediaItem& m : items) {
            state.media.push_back(std::make_shared<const MediaItem>(m));
        }
        session.reset(state);
        doc = std::make_unique<ProjectDocument>(session);
        REQUIRE(doc->saveAs(projectFile));
        manager = std::make_unique<ProxyManager>(
            *doc, ProxyManagerOptions{.untitledProxyRoot = projectFile.parent_path() / "untitled-proxy-root",
                                      .policy = ProxyPolicy::Off,
                                      .transcoder = std::move(transcoder),
                                      .probeHardware = false});
    }
};

/// A fake transcode that writes `content` to the job's output.
ProxyQueue::Transcoder writingTranscoder(std::string content) {
    return [content](const ProxyJob& job, const exporter::ProxyControl&) -> std::expected<exporter::ProxyResult, Error> {
        writeText(job.output, content);
        exporter::ProxyResult r;
        r.file = job.output;
        r.frames = 1;
        return r;
    };
}

bool settle(ProxyManager& pm, std::chrono::seconds timeout = 30s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    int quiet = 0;
    while (std::chrono::steady_clock::now() < until) {
        pm.poll();
        quiet = pm.queue().statuses().empty() ? quiet + 1 : 0;
        if (quiet >= 5) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

ProjectState withSequences(int n) {
    ProjectState state;
    for (int i = 0; i < n; ++i) {
        auto created = edit::addSequence(state, edit::SequenceSettings{});
        REQUIRE(created);
        state = std::move(created->state);
    }
    return state;
}

int sequencesIn(const fs::path& file) {
    auto loaded = io::loadProject(file);
    return loaded ? static_cast<int>(loaded->sequences.size()) : -1;
}

/// Sets an environment variable for the scope.
struct ScopedEnv {
    std::string name;
    std::optional<std::string> old;
    ScopedEnv(std::string n, const std::string& value) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str())) {
            old = v;
        }
        set(value);
    }
    ~ScopedEnv() {
        if (old) {
            set(*old);
        } else {
#ifdef _WIN32
            _putenv_s(name.c_str(), "");
#else
            unsetenv(name.c_str());
#endif
        }
    }
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
};

} // namespace

// ---- P1-1: project-supplied proxy paths ---------------------------------------------------------------------------

TEST_CASE("Proxies (wave-3 P1-1): a project's proxy path outside the managed folder is detached, never deleted",
          "[editor][proxy][wave3]") {
    const fs::path dir = workDir("proxy-project");
    const fs::path original = dir / "media" / "clip.mp4";
    const fs::path victim = dir / "user" / "thesis.docx";
    writeText(original, "the original media");
    writeText(victim, "a valuable document");
    MediaItem m = videoItem(MediaId{1}, original);
    m.locator.proxy = ProxyMedia{pathToUtf8(victim), provenanceOf(m)};
    ProxyProject p({m}, dir / "evil.wabcut");
    p.manager->refresh();
    p.manager->deleteProxies({m.id});
    CHECK(fs::exists(victim));
    CHECK(readText(victim) == "a valuable document");
    CHECK_FALSE(p.manager->proxyFile(m.id)); // detached
    CHECK(fs::exists(original));
}

TEST_CASE("Proxies (wave-3 P1-1): a malicious proxy index cannot point outside its folder", "[editor][proxy][wave3]") {
    const fs::path dir = workDir("proxy-index");
    const fs::path original = dir / "media" / "clip.mp4";
    const fs::path victim = dir / "user" / "thesis.docx";
    const fs::path victim2 = dir / "user" / "taxes.xlsx";
    writeText(original, "the original media");
    writeText(victim, "a valuable document");
    writeText(victim2, "another valuable document");
    const MediaItem m = videoItem(MediaId{1}, original);
    // Relative escape, an absolute path, and the original itself: none is a file the index may own.
    writeIndex(dir / "evil Proxies", m, {"../user/thesis.docx", pathToUtf8(victim2), "../media/clip.mp4"});
    ProxyProject p({m}, dir / "evil.wabcut");
    p.manager->refresh();
    CHECK_FALSE(p.manager->proxyFile(m.id)); // nothing adopted
    p.manager->deleteProxies({m.id});
    CHECK(fs::exists(victim));
    CHECK(fs::exists(victim2));
    CHECK(fs::exists(original));
}

TEST_CASE("Proxies (wave-3 P1-1): a proxy that is the original (another spelling, a hard link, a junction) is refused",
          "[editor][proxy][wave3]") {
    const fs::path dir = workDir("proxy-alias");
    const fs::path original = dir / "media" / "Clip.mp4";
    writeText(original, "the original media");
    const MediaItem base = videoItem(MediaId{1}, original);

    SECTION("proxy == original, spelled in another case") {
        MediaItem m = base;
        std::string upper = pathToUtf8(original);
        for (char& c : upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
#ifdef _WIN32
        m.locator.proxy = ProxyMedia{upper, provenanceOf(m)};
#else
        m.locator.proxy = ProxyMedia{pathToUtf8(original), provenanceOf(m)};
#endif
        ProxyProject p({m}, dir / "alias.wabcut");
        p.manager->refresh();
        CHECK_FALSE(p.manager->proxyFile(m.id));
        p.manager->deleteProxies({m.id});
        CHECK(fs::exists(original));
    }
    SECTION("a hard link to the original in the managed folder, listed in its index") {
        fs::create_directories(dir / "alias Proxies");
        std::error_code ec;
        fs::create_hard_link(original, dir / "alias Proxies" / "Clip.a1b2c3d4.g1.mov", ec);
        REQUIRE_FALSE(ec);
        writeIndex(dir / "alias Proxies", base, {"Clip.a1b2c3d4.g1.mov"});
        ProxyProject p({base}, dir / "alias.wabcut");
        p.manager->refresh();
        CHECK_FALSE(p.manager->proxyFile(base.id)); // an alias of the original is no proxy
    }
    SECTION("the proxy folder is a junction to the originals' folder") {
        if (!makeJunction(dir / "alias Proxies", dir / "media")) {
            WARN("junctions are not available here");
            return;
        }
        writeIndex(dir / "alias Proxies", base, {"Clip.mp4"}); // lands in the media folder through the junction
        ProxyProject p({base}, dir / "alias.wabcut");
        p.manager->refresh();
        p.manager->deleteProxies({base.id});
        CHECK(fs::exists(original));
        CHECK(readText(original) == "the original media");
        std::error_code ec;
        fs::remove(dir / "alias Proxies", ec); // the junction itself
    }
    SECTION("a file symlink to the original as the project's proxy") {
        std::error_code ec;
        fs::create_symlink(original, dir / "link.mov", ec);
        if (ec) {
            WARN("file symlinks need a privilege here: " << ec.message());
            return;
        }
        MediaItem m = base;
        m.locator.proxy = ProxyMedia{pathToUtf8(dir / "link.mov"), provenanceOf(m)};
        ProxyProject p({m}, dir / "alias.wabcut");
        p.manager->refresh();
        CHECK_FALSE(p.manager->proxyFile(m.id));
        p.manager->deleteProxies({m.id});
        CHECK(fs::exists(original));
    }
}

TEST_CASE("Proxies (wave-3 P1-1): regeneration deletes only the previous proxy Wabcut made", "[editor][proxy][wave3]") {
    const fs::path dir = workDir("proxy-regenerate");
    const fs::path original = dir / "media" / "clip.mp4";
    const fs::path victim = dir / "user" / "thesis.docx";
    writeText(original, "the original media");
    writeText(victim, "a valuable document");
    MediaItem m = videoItem(MediaId{1}, original);
    m.locator.proxy = ProxyMedia{pathToUtf8(victim), provenanceOf(m)};
    ProxyProject p({m}, dir / "regen.wabcut", writingTranscoder("generated proxy"));
    p.manager->refresh();
    // A new proxy replaces the attached one: the attached file is left where it is.
    REQUIRE(p.manager->generate({m.id}) == 1);
    REQUIRE(settle(*p.manager));
    const auto made = p.manager->proxyFile(m.id);
    REQUIRE(made);
    CHECK(fs::exists(victim));
    CHECK(readText(*made) == "generated proxy");
    // Wabcut's own proxy is deleted when a newer one replaces it, and by Delete proxies.
    exporter::ProxySettings other = p.manager->settings();
    other.divisor = other.divisor == 2 ? 4 : 2;
    p.manager->setSettings(other);
    REQUIRE(p.manager->generate({m.id}) == 1);
    REQUIRE(settle(*p.manager));
    const auto second = p.manager->proxyFile(m.id);
    REQUIRE(second);
    CHECK(*second != *made);
    CHECK_FALSE(fs::exists(*made));
    CHECK(p.manager->deleteProxies({m.id}) == 1);
    CHECK_FALSE(fs::exists(*second));
    CHECK(fs::exists(victim));
    CHECK(fs::exists(original));
}

// ---- P1-3: Recover -----------------------------------------------------------------------------------------------

TEST_CASE("Recover (wave-3 P1-3): the recovered revision is autosaved again; a crash right after keeps it",
          "[editor][recovery][wave3]") {
    const fs::path dir = workDir("recover");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(withSequences(1), project, io::SaveOptions{0}));
    // The crash left an autosave with work the project file does not have.
    REQUIRE(io::saveProject(withSequences(2), io::autosavePath(project), io::SaveOptions{0}));
    {
        EditSession session;
        ProjectDocument doc(session);
        REQUIRE(doc.recover(io::autosavePath(project), project));
        CHECK(doc.isDirty());
        const auto wrote = doc.autosave(); // no further edit
        REQUIRE(wrote);
        CHECK(*wrote); // the recovered revision is eligible for autosave
    } // a crash here: nothing removes anything
    EditSession session;
    ProjectDocument doc(session);
    const auto files = doc.recoveryFiles(project);
    REQUIRE_FALSE(files.empty());
    CHECK(files.front().kind == RecoveryFile::Kind::Autosave);
    CHECK(files.front().loadable);
    CHECK(sequencesIn(files.front().file) == 2);
}

TEST_CASE("Recover (wave-3 P1-3): a failed autosave keeps the recovery file", "[editor][recovery][wave3]") {
    const fs::path dir = workDir("recover-failure");
    const fs::path project = dir / "film.wabcut";
    REQUIRE(io::saveProject(withSequences(1), project, io::SaveOptions{0}));
    const fs::path recovery = io::autosavePath(project);
    REQUIRE(io::saveProject(withSequences(2), recovery, io::SaveOptions{0}));
    fs::path blocked = recovery;
    blocked += ".tmp";
    fs::create_directories(blocked); // the atomic write's temporary file cannot be created: disk full / no permission
    EditSession session;
    ProjectDocument doc(session);
    REQUIRE(doc.recover(recovery, project));
    CHECK_FALSE(doc.autosave());
    CHECK(sequencesIn(recovery) == 2);
    fs::remove_all(blocked);
    REQUIRE(doc.autosave());
    CHECK(sequencesIn(recovery) == 2);
}

TEST_CASE("Recover (wave-3 P1-3): an untitled recovery file is retired only after its new session autosaved",
          "[editor][recovery][wave3]") {
    const fs::path dir = workDir("recover-untitled");
    const fs::path oldFile = dir / "Autosave" / "Untitled-100-1.wabcut.autosave";
    fs::create_directories(oldFile.parent_path());
    REQUIRE(io::saveProject(withSequences(2), oldFile, io::SaveOptions{0}));
    SECTION("the replacement write fails: the file stays") {
        writeText(dir / "blocked", "a file where the autosave folder should be");
        EditSession session;
        ProjectDocument doc(session, {.untitledAutosaveDir = dir / "blocked"});
        REQUIRE(doc.recover(oldFile, {}));
        CHECK_FALSE(doc.autosave());
        CHECK(sequencesIn(oldFile) == 2);
    }
    SECTION("the replacement is written: the old session's file goes") {
        EditSession session;
        ProjectDocument doc(session, {.untitledAutosaveDir = dir / "Autosave"});
        REQUIRE(doc.recover(oldFile, {}));
        const auto wrote = doc.autosave();
        REQUIRE(wrote);
        CHECK(*wrote);
        CHECK(sequencesIn(doc.autosaveFile()) == 2);
        CHECK(doc.autosaveFile() != oldFile);
        CHECK_FALSE(fs::exists(oldFile));
    }
}

// ---- P2-7: untitled autosave rotation ----------------------------------------------------------------------------

TEST_CASE("Untitled autosaves (wave-3 P2-7): every rotation/write interruption leaves a listed, loadable copy",
          "[editor][recovery][wave3]") {
    const fs::path dir = workDir("untitled-rotation");
    const fs::path autosaves = dir / "Autosave";
    fs::create_directories(autosaves);
    // Interrupted after the newest copy was rotated away, before its replacement was written: numbered copies only.
    REQUIRE(io::saveProject(withSequences(3), autosaves / "Untitled-111-1.wabcut.autosave2", io::SaveOptions{0}));
    REQUIRE(io::saveProject(withSequences(2), autosaves / "Untitled-111-1.wabcut.autosave3", io::SaveOptions{0}));
    // Interrupted mid-write (no atomic write): a torn newest copy, an intact older one, a temporary leftover.
    writeText(autosaves / "Untitled-222-1.wabcut.autosave", "{\"format\": \"wabcut\", torn");
    REQUIRE(io::saveProject(withSequences(4), autosaves / "Untitled-222-1.wabcut.autosave2", io::SaveOptions{0}));
    writeText(autosaves / "Untitled-222-1.wabcut.autosave.tmp", "partial");

    const std::vector<RecoveryFile> listed = ProjectDocument::untitledRecoveryFiles(autosaves);
    REQUIRE(listed.size() == 2); // one per session
    std::set<std::string> names;
    for (const RecoveryFile& r : listed) {
        CHECK(r.loadable);
        names.insert(pathToUtf8(r.file.filename()));
    }
    CHECK(names == std::set<std::string>{"Untitled-111-1.wabcut.autosave2", "Untitled-222-1.wabcut.autosave2"});
    CHECK(sequencesIn(autosaves / "Untitled-111-1.wabcut.autosave2") == 3); // the latest loadable copy of 111
    // The dashboard lists them too.
    int untitled = 0;
    for (const RecoveryEntry& e : recoveryEntries({}, autosaves)) {
        untitled += e.kind == RecoveryEntry::Kind::UntitledAutosave ? 1 : 0;
    }
    CHECK(untitled == 2);
}

TEST_CASE("Untitled autosaves (wave-3 P2-7): a failed write keeps the current copy where it is", "[editor][recovery][wave3]") {
    const fs::path dir = workDir("untitled-write-failure");
    EditSession session;
    session.reset(withSequences(1));
    ProjectDocument doc(session, {.untitledAutosaveDir = dir / "Autosave"});
    session.reset(withSequences(2));
    REQUIRE(doc.autosave());
    const fs::path current = doc.autosaveFile();
    REQUIRE(sequencesIn(current) == 2);
    session.reset(withSequences(3));
    fs::path blocked = current;
    blocked += ".tmp";
    fs::create_directories(blocked); // the replacement cannot be written
    CHECK_FALSE(doc.autosave());
    CHECK(sequencesIn(current) == 2); // still there, still loadable
    CHECK(ProjectDocument::untitledRecoveryFiles(dir / "Autosave").size() == 1);
    fs::remove_all(blocked);
    REQUIRE(doc.autosave());
    CHECK(sequencesIn(current) == 3);
    fs::path older = current;
    older += "2";
    CHECK(sequencesIn(older) == 2); // the previous copy rotated once the new one was safe
    CHECK((fs::last_write_time(older) <= fs::last_write_time(current))); // and it does not look newer than it is
    CHECK(ProjectDocument::untitledRecoveryFiles(dir / "Autosave").front().file == current);
}

// ---- P2-11: Open Recovery Copy -----------------------------------------------------------------------------------

TEST_CASE("Open Recovery Copy (wave-3 P2-11): relocated project, autosave and backup copies find their relative media",
          "[editor][recovery][wave3]") {
    const fs::path dir = workDir("recovery-copy");
    const fs::path moved = dir / "moved";
    writeText(moved / "Media" / "clip.bin", "media next to the copies");
    MediaItem media;
    media.name = "clip";
    media.locator.path = "Z:/no-such-drive/old-place/Media/clip.bin"; // the saved absolute path is gone
    media.locator.relativePath = "Media/clip.bin";
    auto added = edit::addMedia(withSequences(1), media);
    REQUIRE(added);
    const fs::path project = moved / "film.wabcut";
    std::vector<fs::path> copies{project, io::autosavePath(project), io::backupPath(project, 1)};
    fs::path autosave2 = io::autosavePath(project);
    autosave2 += "2";
    copies.push_back(autosave2);
    for (const fs::path& f : copies) {
        REQUIRE(io::saveProject(added->state, f, io::SaveOptions{0}));
    }
    for (const fs::path& f : copies) {
        INFO(pathToUtf8(f.filename()));
        EditSession session;
        ProjectDocument doc(session);
        REQUIRE(doc.recover(f, {}));
        CHECK(doc.isUntitled());
        CHECK(doc.isDirty());
        CHECK(doc.offlineMedia().empty());
        const auto status = doc.mediaStatus();
        REQUIRE(status.size() == 1);
        std::error_code ec;
        CHECK(fs::equivalent(pathFromUtf8(status.front().path), moved / "Media" / "clip.bin", ec));
    }
}

// ---- P1-4: Collect Files -----------------------------------------------------------------------------------------

TEST_CASE("Collect Files (wave-3 P1-4): names that differ only by case never overwrite each other",
          "[editor][capsule][wave3]") {
    const fs::path dir = workDir("capsule-case");
    ProjectState state;
    const auto addMediaFile = [&](const fs::path& file, const std::string& content, std::optional<fs::path> proxy) {
        writeText(file, content);
        MediaItem m;
        m.name = pathToUtf8(file.filename());
        m.locator.path = pathToUtf8(file);
        m.info.path = m.locator.path;
        m.locator.fingerprint = fingerprintFile(file).value();
        if (proxy) {
            writeText(*proxy, "proxy of " + content);
            m.locator.proxy = ProxyMedia{pathToUtf8(*proxy), ProxyProvenance{}};
        }
        auto added = edit::addMedia(state, m);
        REQUIRE(added);
        state = std::move(added->state);
    };
    addMediaFile(dir / "a" / "A.wav", "first media", dir / "p1" / "P.mov");
    addMediaFile(dir / "b" / "a.wav", "SECOND different media", dir / "p2" / "p.mov");
    for (const auto& [file, content] : {std::pair{dir / "l1" / "Look.cube", std::string("LUT one")},
                                        std::pair{dir / "l2" / "look.cube", std::string("lut TWO")}}) {
        writeText(file, content);
        ProjectAsset asset;
        asset.kind = std::string(kAssetKindLut);
        asset.name = pathToUtf8(file.filename());
        asset.locator.path = pathToUtf8(file);
        asset.locator.fingerprint = fingerprintFile(file).value();
        auto added = edit::addAsset(state, asset);
        REQUIRE(added);
        state = std::move(added->state);
    }
    writeText(dir / "f1" / "X.ttf", "font one");
    writeText(dir / "f2" / "x.ttf", "FONT two");

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "collision";
    options.excludeUnused = false;
    options.includeProxies = true;
    options.verify = true;
    options.extras = {{dir / "f1" / "X.ttf", "Fonts/X.ttf", "font"}, {dir / "f2" / "x.ttf", "fonts/x.ttf", "font"}};
    const auto result = collectProject(state, {}, options);
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result);
    CHECK(result->verified);
    CHECK(result->fingerprintMismatches == 0);
    CHECK(result->offlineAfterReopen == 0);
    // Every collected file is its own source, byte for byte.
    int checked = 0;
    std::set<std::string> folded;
    for (const CapsuleFile& f : result->files) {
        if (f.role == "project") {
            continue;
        }
        INFO(pathToUtf8(f.relative));
        CHECK(readText(result->folder / f.relative) == readText(pathFromUtf8(f.source)));
        std::string key = pathToUtf8(f.relative);
        for (char& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        CHECK(folded.insert(key).second); // no two files share a name, whatever the case
        ++checked;
    }
    CHECK(checked == 8); // 2 media, 2 proxies, 2 LUTs, 2 fonts
}

TEST_CASE("Collect Files (wave-3 P1-4): a capsule that cannot be verified is an error, not a success",
          "[editor][capsule][wave3]") {
    const fs::path dir = workDir("capsule-unverified");
    writeText(dir / "media" / "clip.wav", "media");
    MediaItem m;
    m.name = "clip.wav";
    m.locator.path = pathToUtf8(dir / "media" / "clip.wav");
    m.locator.fingerprint = fingerprintFile(dir / "media" / "clip.wav").value();
    auto added = edit::addMedia(ProjectState{}, m);
    REQUIRE(added);
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "unverified";
    options.excludeUnused = false;
    options.verify = true;
    // The verification copy cannot be made: the temporary folder is a file.
    writeText(dir / "not-a-folder", "x");
    std::optional<std::expected<CapsuleResult, Error>> result;
    {
        const std::string blocked = pathToUtf8(dir / "not-a-folder" / "tmp");
        ScopedEnv tmp("TMP", blocked);
        ScopedEnv temp("TEMP", blocked);
        ScopedEnv tmpdir("TMPDIR", blocked);
        result = collectProject(added->state, {}, options);
    }
    REQUIRE(result);
    CHECK_FALSE(result->has_value());
    CHECK_FALSE(fs::exists(dir / "out" / "unverified")); // no capsule that looks finished
}

// ---- Audit: the cache inspector cleans folders a project's location decides ---------------------------------------

TEST_CASE("Cache cleaning (wave-3 audit): never through a linked cache folder; in-use files in any spelling survive",
          "[editor][proxy][wave3]") {
    const fs::path dir = workDir("cache-clean");
    writeText(dir / "Videos" / "holiday.mov", "the user's own video");
    writeText(dir / "proxies" / "clip.a1b2c3d4.g1.mov", "an unused proxy");
    writeText(dir / "proxies" / "used.a1b2c3d4.g1.mov", "a proxy in use");
    SECTION("the project's proxy folder is a junction to the user's videos") {
        if (!makeJunction(dir / "film Proxies", dir / "Videos")) {
            WARN("junctions are not available here");
            return;
        }
        CacheInspector inspector;
        inspector.setLocations({{"proxies", "Proxies", dir / "film Proxies", 1, {".mov"}}});
        inspector.clean("proxies");
        inspector.enforceLimits();
        CHECK(fs::exists(dir / "Videos" / "holiday.mov"));
        std::error_code ec;
        fs::remove(dir / "film Proxies", ec);
    }
    SECTION("a file in use is kept whatever the case of the path it is known by") {
        CacheInspector inspector;
        inspector.setLocations({{"proxies", "Proxies", dir / "proxies", 0, {".mov"}}});
        std::string spelled = pathToUtf8((dir / "proxies" / "used.a1b2c3d4.g1.mov").lexically_normal());
#ifdef _WIN32
        for (char& c : spelled) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
#endif
        inspector.clean("proxies", {pathFromUtf8(spelled)});
        CHECK(fs::exists(dir / "proxies" / "used.a1b2c3d4.g1.mov"));
        CHECK_FALSE(fs::exists(dir / "proxies" / "clip.a1b2c3d4.g1.mov"));
    }
}
