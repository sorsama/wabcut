// I.4: the recovery dashboard (every version of a project with its summary and poster; a version opens as an
// untitled copy) and the portable project capsule (Collect Files: whole and trimmed media, the manifest, verification
// by reopening elsewhere, frame-exact trimmed clips, cancel and refusals leave nothing behind).

#include <wabcut/editor/Capsule.h>

#include "PathSafety.h"
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/Recovery.h>
#include <wabcut/editor/SourceEdit.h>

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TimeRemap.h>

#include <nlohmann/json.hpp>

#include <QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path testMedia(const char* name) {
    return fs::path(WABCUT_TEST_MEDIA_DIR) / name;
}

bool haveClips() {
    return fs::exists(testMedia("h264_1080p60_aac.mp4")) && fs::exists(testMedia("prores_422.mov")) &&
           fs::exists(testMedia("sine_48k.wav"));
}

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

MediaId importOne(EditSession& session, const fs::path& file, const fs::path& projectFile) {
    const std::vector<fs::path> files{file};
    const MediaImportResult r = importMediaFiles(session, files, {.projectFile = projectFile});
    REQUIRE(r.failed.empty());
    REQUIRE(r.added.size() == 1);
    return r.added.front();
}

ClipId drop(EditSession& session, SequenceId seq, MediaId media, Ticks at, std::optional<TimeRange> range = {}) {
    auto placed = dropMedia(session, {.media = media, .sequence = seq, .at = at, .sourceRange = range});
    REQUIRE(placed);
    REQUIRE_FALSE(placed->placed.empty());
    return placed->placed.front();
}

/// Applies `fn` to clip `id` of sequence `seq` (one undo step).
void editClip(EditSession& session, SequenceId seq, ClipId id, const std::function<void(Clip&)>& fn) {
    auto r = session.apply("Edit clip", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        for (SequencePtr& s : next.sequences) {
            if (!s || s->id != seq) {
                continue;
            }
            auto copy = std::make_shared<Sequence>(*s);
            for (TrackPtr& t : copy->videoTracks) {
                std::vector<Clip> clips(t->clips.begin(), t->clips.end());
                for (Clip& c : clips) {
                    if (c.id == id) {
                        fn(c);
                    }
                }
                auto track = std::make_shared<Track>(*t);
                track->clips = ClipList(std::move(clips));
                t = std::move(track);
            }
            s = std::move(copy);
        }
        return next;
    });
    REQUIRE(r);
}

struct Project {
    fs::path dir;
    fs::path file;
    SequenceId sequence;
    MediaId h264;
    MediaId prores;
    MediaId unused;
    ClipId h264Clip;
};

/// A saved project over copies of the clips: H.264 used from 2 s to 3 s (opacity keyed in media time at 2.2 s and
/// 2.8 s), ProRes whole, a WAV imported but not used.
Project makeProject(EditSession& session, ProjectDocument& doc, const fs::path& dir) {
    Project p;
    p.dir = dir;
    fs::create_directories(dir / "media");
    for (const char* name : {"h264_1080p60_aac.mp4", "prores_422.mov", "sine_48k.wav"}) {
        fs::copy_file(testMedia(name), dir / "media" / name, fs::copy_options::overwrite_existing);
    }
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    p.sequence = created->id;
    session.reset(std::move(created->state));
    p.file = dir / "project.wabcut";
    REQUIRE(doc.saveAs(p.file));
    p.h264 = importOne(session, dir / "media" / "h264_1080p60_aac.mp4", p.file);
    p.prores = importOne(session, dir / "media" / "prores_422.mov", p.file);
    p.unused = importOne(session, dir / "media" / "sine_48k.wav", p.file);
    p.h264Clip = drop(session, p.sequence, p.h264, Ticks{}, TimeRange::fromStartEnd(Ticks::fromSeconds(2), Ticks::fromSeconds(3)));
    drop(session, p.sequence, p.prores, Ticks::fromSeconds(1));
    editClip(session, p.sequence, p.h264Clip, [](Clip& c) {
        ClipProperties props = c.properties();
        Curve curve;
        curve.keys = {Keyframe{Ticks{kTicksPerSecond * 22 / 10}, 0.2}, Keyframe{Ticks{kTicksPerSecond * 28 / 10}, 0.9}};
        props[ParamId::Opacity] = ParamValue(curve);
        c.setProperties(props);
    });
    REQUIRE(doc.save());
    return p;
}

int barcodeOf(const media::FrameHandle& f) {
    const media::CpuPlanes* cpu = f.cpu();
    if (!cpu) {
        return -1;
    }
    int index = 0;
    for (int bit = 0; bit < 16 && bit * 32 + 16 < f.width; ++bit) {
        const media::Plane& p = cpu->planes[0];
        int luma = 0;
        if (media::pixelFormatBitDepth(f.format) > 8) {
            std::uint16_t v = 0;
            std::memcpy(&v, p.data + static_cast<std::ptrdiff_t>(16) * p.stride + 2 * (bit * 32 + 16), 2);
            luma = f.format == media::PixelFormat::P010 ? v >> 8 : v >> 2;
        } else {
            luma = p.data[static_cast<std::ptrdiff_t>(16) * p.stride + bit * 32 + 16];
        }
        index |= (luma > 128 ? 1 : 0) << bit;
    }
    return index;
}

const Clip& clipOf(const ProjectState& s, SequenceId seq, ClipId id) {
    const auto found = s.findSequence(seq)->findClip(id);
    REQUIRE(found);
    return found->clip();
}

} // namespace

TEST_CASE("Recovery: every version is listed with its summary; a version opens as an untitled copy", "[editor][recovery]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("recovery-dashboard");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir); // saved with 2 video clips (+ their audio)
    const int savedClips = summarizeState(*session.snapshot()).clips;
    // Two more saves rotate backups; an edit after the last save goes only into the autosave.
    std::this_thread::sleep_for(20ms);
    drop(session, p.sequence, p.prores, Ticks::fromSeconds(5));
    REQUIRE(doc.save()); // .bak1 = the first version
    std::this_thread::sleep_for(20ms);
    drop(session, p.sequence, p.prores, Ticks::fromSeconds(8));
    REQUIRE(doc.save()); // .bak1 = second, .bak2 = first
    std::this_thread::sleep_for(20ms);
    drop(session, p.sequence, p.prores, Ticks::fromSeconds(11));
    REQUIRE(doc.autosave());
    const int autosaved = summarizeState(*session.snapshot()).clips;

    const std::vector<RecoveryEntry> versions = recoveryEntries(p.file, {});
    REQUIRE(versions.size() >= 4);
    CHECK(versions[0].kind == RecoveryEntry::Kind::Current);
    CHECK(versions[1].kind == RecoveryEntry::Kind::Autosave);
    CHECK(versions[1].newerThanProject);
    CHECK(versions[1].summary.clips == autosaved);
    const auto backup = [&](int n) -> const RecoveryEntry* {
        for (const RecoveryEntry& v : versions) {
            if (v.kind == RecoveryEntry::Kind::Backup && v.index == n) {
                return &v;
            }
        }
        return nullptr;
    };
    REQUIRE(backup(1));
    REQUIRE(backup(2));
    CHECK(backup(2)->summary.clips == savedClips);
    CHECK(backup(1)->summary.clips > backup(2)->summary.clips);
    CHECK(versions[0].summary.clips > backup(1)->summary.clips);
    CHECK(versions[0].summary.sequences == 1);
    CHECK(versions[0].summary.duration > Ticks::fromSeconds(10));
    CHECK(versions[0].summary.loadable);
    for (std::size_t i = 2; i < versions.size(); ++i) {
        CHECK(versions[i - 1].summary.modified >= versions[i].summary.modified); // newest first after Current
    }

    // Open the oldest backup as a copy: untitled, with unsaved changes; the project file is untouched.
    const auto before = fs::last_write_time(p.file);
    REQUIRE(doc.recover(backup(2)->file, {}));
    CHECK(doc.isUntitled());
    CHECK(doc.isDirty());
    CHECK(summarizeState(*session.snapshot()).clips == savedClips);
    CHECK((fs::last_write_time(p.file) == before)); // a bool: libc++'s file times (__int128) do not print
    CHECK(summarizeProjectFile(p.file).clips == versions[0].summary.clips);

    // An unreadable version is listed with why.
    { std::ofstream(io::backupPath(p.file, 3)) << "not a project"; }
    bool broken = false;
    for (const RecoveryEntry& v : recoveryEntries(p.file, {})) {
        broken = broken || (v.kind == RecoveryEntry::Kind::Backup && v.index == 3 && !v.summary.loadable && !v.summary.error.empty());
    }
    CHECK(broken);
}

TEST_CASE("Recovery: the poster frame is the export renderer's picture", "[editor][recovery][gpu]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    // The renderer needs a QGuiApplication (shaders, the platform's QRhi); no window is created.
    static int argc = 1;
    static char arg0[] = "wabcut_editor_tests";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance()) {
        static QGuiApplication app(argc, argv);
    }
    const fs::path dir = workDir("recovery-poster");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir);
    auto poster = renderPoster(*session.snapshot(), p.file, 320);
    REQUIRE(poster);
    CHECK(poster->width == 320);
    CHECK(poster->height == 180);
    std::set<std::uint32_t> colours;
    for (std::size_t i = 0; i + 3 < poster->rgba.size(); i += 4 * 7) {
        colours.insert((poster->rgba[i] >> 4) << 8 | (poster->rgba[i + 1] >> 4) << 4 | (poster->rgba[i + 2] >> 4));
    }
    CHECK(colours.size() > 20); // a picture, not black
}

TEST_CASE("Capsule: whole files; reopened elsewhere nothing is offline; the manifest matches", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-whole");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Job";
    options.wabcutVersion = "test";
    std::vector<CapsuleProgress> seen;
    auto r = collectProject(session.stateForSave(), p.file, options, {}, [&](const CapsuleProgress& pr) { seen.push_back(pr); });
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    INFO("offline after reopen " << r->offlineAfterReopen << ", mismatches " << r->fingerprintMismatches << ", notes "
                                 << (r->notes.empty() ? std::string() : r->notes.front()));
    CHECK(r->verified);
    CHECK(r->offlineAfterReopen == 0);
    CHECK(r->fingerprintMismatches == 0);
    CHECK(r->mediaFiles == 2); // the WAV is not used
    CHECK(r->trimmedFiles == 0);
    CHECK(r->folder == dir / "out" / "Job");
    CHECK(fs::is_regular_file(r->folder / "Job.wabcut"));
    CHECK(fs::is_regular_file(r->folder / "Media" / "h264_1080p60_aac.mp4"));
    CHECK(fs::file_size(r->folder / "Media" / "prores_422.mov") == fs::file_size(testMedia("prores_422.mov")));
    CHECK_FALSE(seen.empty());
    CHECK(seen.back().phase == "done");
    for (const auto& entry : fs::directory_iterator(dir / "out")) {
        CHECK(entry.path().filename() == "Job"); // no partial folder left
    }

    // The manifest: every file, its size and fingerprint.
    nlohmann::json manifest;
    {
        std::ifstream in(r->manifest);
        manifest = nlohmann::json::parse(in);
    }
    CHECK(manifest["format"] == "wabcut-capsule");
    CHECK(manifest["wabcutVersion"] == "test");
    int media = 0;
    for (const auto& f : manifest["files"]) {
        const fs::path file = r->folder / pathFromUtf8(f["path"].get<std::string>());
        REQUIRE(fs::is_regular_file(file));
        CHECK(static_cast<std::int64_t>(fs::file_size(file)) == f["bytes"].get<std::int64_t>());
        CHECK(fingerprintFile(file)->sampleHash == f["sampleHash"].get<std::string>());
        media += f["role"] == "media" ? 1 : 0;
    }
    CHECK(media == 2);

    // Moved somewhere else and the originals gone: it opens with nothing offline, the same media.
    const fs::path moved = dir / "elsewhere" / "Job";
    fs::create_directories(moved.parent_path());
    fs::rename(r->folder, moved);
    fs::remove_all(dir / "source" / "media");
    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(moved / "Job.wabcut"));
    CHECK(copy.offlineMedia().empty());
    const ProjectState& state = *reopened.snapshot();
    CHECK(state.findMedia(p.unused) == nullptr);
    REQUIRE(state.findMedia(p.h264));
    CHECK(state.findMedia(p.h264)->locator.fingerprint.sameContentAs(fingerprintFile(testMedia("h264_1080p60_aac.mp4")).value()));
    CHECK(clipOf(state, p.sequence, p.h264Clip).sourceIn == Ticks::fromSeconds(2));
}

TEST_CASE("Capsule: trimmed to the used ranges, clips and curves follow frame for frame", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-trimmed");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    const ProjectState original = session.stateForSave();
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Trimmed";
    options.trim = true;
    options.handles = Ticks{kTicksPerSecond / 2};
    CHECK(capsuleBytesNeeded(original, p.file, options) <
          capsuleBytesNeeded(original, p.file, CapsuleOptions{.destination = options.destination}));
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    INFO("offline after reopen " << r->offlineAfterReopen << ", mismatches " << r->fingerprintMismatches << ", notes "
                                 << (r->notes.empty() ? std::string() : r->notes.front()));
    CHECK(r->verified);
    CHECK(r->trimmedFiles == 2);
    CHECK(fs::file_size(r->folder / "Media" / "h264_1080p60_aac.mp4") < fs::file_size(testMedia("h264_1080p60_aac.mp4")) / 2);

    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    CHECK(copy.offlineMedia().empty());
    const ProjectState& state = *reopened.snapshot();
    const Clip& before = clipOf(original, p.sequence, p.h264Clip);
    const Clip& after = clipOf(state, p.sequence, p.h264Clip);
    const Ticks shift = before.sourceIn - after.sourceIn;
    CHECK(shift > Ticks{});
    CHECK(after.sourceIn >= Ticks{});
    // Keyframes moved with the media: the same opacity at the same timeline time.
    for (const Ticks t : {Ticks{}, Ticks{kTicksPerSecond / 3}, Ticks{kTicksPerSecond * 9 / 10}}) {
        CHECK(after.parameterAt(ParamId::Opacity, t) == before.parameterAt(ParamId::Opacity, t));
    }
    // Every frame the clip shows is the same picture in the copy.
    auto a = media::VideoDecoder::open(pathToUtf8(testMedia("h264_1080p60_aac.mp4")), -1, media::DecoderOptions{.allowHardware = false});
    auto b = media::VideoDecoder::open(state.findMedia(p.h264)->locator.path, -1, media::DecoderOptions{.allowHardware = false});
    REQUIRE(a);
    REQUIRE(b);
    int frames = 0;
    for (Ticks t = before.start(); t < before.end(); t = t + Ticks{kTicksPerSecond / 60}) {
        auto fa = (*a)->frameAt(before.sourceTimeAt(t));
        auto fb = (*b)->frameAt(after.sourceTimeAt(t));
        REQUIRE(fa);
        REQUIRE(fb);
        CHECK(barcodeOf(*fb) == barcodeOf(*fa));
        ++frames;
    }
    CHECK(frames == 60);
}

// Codex wave-4 P1-7: a remapped clip's sourceIn and keyframes are curve times and its remap keys hold media positions.
// Trimming must keep the media the curve shows (a 200 % ramp shows twice its length) and move only the key positions.
TEST_CASE("Capsule: trimmed remapped clips show the same frames (ramp, hold, reverse)", "[editor][capsule][remap]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-remapped");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    // The clip spans curve times [2 s, 3 s): 200 % forward to 2.4 s, a hold to 2.6 s, 150 % reverse to 2.8 s, then 400 %
    // forward, which shows media up to 3.3 s, past the clip's [2 s, 3 s) of curve time. The keys at 1 s and 5 s bound no
    // segment the clip shows (the far one points at 8 s of media).
    editClip(session, p.sequence, p.h264Clip, [](Clip& c) {
        ClipContent content = c.content();
        Curve remap;
        const auto key = [](double seconds, double position) {
            return Keyframe{Ticks{static_cast<std::int64_t>(seconds * kTicksPerSecond)}, position * kTicksPerSecond};
        };
        remap.keys = {key(1.0, 1.0), key(2.0, 2.0), key(2.4, 2.8), key(2.6, 2.8), key(2.8, 2.5), key(3.0, 3.3), key(5.0, 8.0)};
        content.timeRemap = remap;
        c.setContent(std::move(content));
    });
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();
    const Clip& before = clipOf(original, p.sequence, p.h264Clip);
    REQUIRE(before.isRemapped());

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Remapped";
    options.trim = true;
    options.handles = Ticks{};
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->verified);

    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    const ProjectState& state = *reopened.snapshot();
    const Clip& after = clipOf(state, p.sequence, p.h264Clip);
    REQUIRE(after.isRemapped());
    CHECK(after.sourceIn == before.sourceIn); // a curve time, not media time
    CHECK_FALSE(remapProblem(*after.timeRemap()).has_value());
    for (const Ticks t : {Ticks{}, Ticks{kTicksPerSecond / 3}, Ticks{kTicksPerSecond * 9 / 10}}) {
        CHECK(after.parameterAt(ParamId::Opacity, t) == before.parameterAt(ParamId::Opacity, t));
    }
    // Every frame the clip shows is the same picture in the copy.
    auto a = media::VideoDecoder::open(pathToUtf8(testMedia("h264_1080p60_aac.mp4")), -1, media::DecoderOptions{.allowHardware = false});
    auto b = media::VideoDecoder::open(state.findMedia(p.h264)->locator.path, -1, media::DecoderOptions{.allowHardware = false});
    REQUIRE(a);
    REQUIRE(b);
    int frames = 0;
    for (Ticks t = before.start(); t < before.end(); t = t + Ticks{kTicksPerSecond / 60}) {
        auto fa = (*a)->frameAt(before.sourceTimeAt(t));
        auto fb = (*b)->frameAt(after.sourceTimeAt(t));
        REQUIRE(fa);
        INFO("timeline " << t.toSeconds() << " s: source " << before.sourceTimeAt(t).toSeconds() << " s, copy "
                         << after.sourceTimeAt(t).toSeconds() << " s");
        REQUIRE(fb);
        CHECK(barcodeOf(*fb) == barcodeOf(*fa));
        ++frames;
    }
    CHECK(frames == 60);
}

TEST_CASE("Capsule: cancelled or refused, nothing is left behind", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-cancel");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Cancelled";
    std::stop_source stop;
    int copied = 0;
    auto r = collectProject(session.stateForSave(), p.file, options, stop.get_token(), [&](const CapsuleProgress& pr) {
        if (pr.bytesDone > 0 && ++copied == 2) {
            stop.request_stop(); // mid-copy
        }
    });
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Cancelled);
    CHECK((!fs::exists(dir / "out") || fs::is_empty(dir / "out")));

    // An existing capsule folder is not overwritten.
    fs::create_directories(dir / "out" / "Taken");
    { std::ofstream(dir / "out" / "Taken" / "keep.txt") << "mine"; }
    options.name = "Taken";
    auto taken = collectProject(session.stateForSave(), p.file, options);
    REQUIRE_FALSE(taken);
    CHECK(taken.error().code == ErrorCode::InvalidArgument);
    CHECK(fs::is_regular_file(dir / "out" / "Taken" / "keep.txt"));

    // Offline media refuses before anything is written.
    fs::rename(dir / "source" / "media" / "prores_422.mov", dir / "source" / "moved.mov");
    REQUIRE(doc.refreshMediaStatus());
    options.name = "Offline";
    auto offline = collectProject(session.stateForSave(), p.file, options);
    REQUIRE_FALSE(offline);
    CHECK(offline.error().code == ErrorCode::NotFound);
    CHECK_FALSE(fs::exists(dir / "out" / "Offline"));
}

// I.7 portability: a capsule moves between computers, so the names it creates must be valid on Windows, macOS and Linux
// and differ in more than case or Unicode normalisation; this computer's own comparisons follow its file system.
TEST_CASE("Path rules: portable names for capsules, this file system's folding for local paths", "[editor][capsule][paths]") {
    using namespace wabcut::editor::paths;
    CHECK(portableName("CON.wav") == "CON_.wav");
    CHECK(portableName("aux") == "aux_");
    CHECK(portableName("lpt9.txt") == "lpt9_.txt");
    CHECK(portableName("COM10.wav") == "COM10.wav"); // only COM1..COM9 are devices
    CHECK(portableName("a:b?.mov") == "a_b_.mov");
    CHECK(portableName("take 1. ") == "take 1");
    CHECK(portableName("...") == "_");
    CHECK(portableName("Caf\xC3\xA9.mov") == "Caf\xC3\xA9.mov");
    // NFC and NFD spellings of "Cafe" with an acute e, and any case, are one name in a capsule.
    CHECK(portableNameKey("Caf\xC3\xA9.mov") == portableNameKey("CAFE\xCC\x81.MOV"));
    CHECK(portableNameKey("\xC3\x89" "cole") == portableNameKey("\xC3\xA9" "cole")); // hex escapes end at "c"
    CHECK(portableNameKey("a.mov") != portableNameKey("b.mov"));
#if defined(_WIN32)
    CHECK(foldName("Clip.MOV") == foldName("clip.mov"));
#elif defined(__APPLE__)
    CHECK(foldName("Clip.MOV") == foldName("clip.mov"));
    CHECK(foldName("Caf\xC3\xA9") == foldName("Cafe\xCC\x81"));
#else
    CHECK(foldName("Clip.MOV") != foldName("clip.mov")); // Linux compares bytes
#endif
    CHECK_FALSE(isPlainName("../escape"));
    CHECK_FALSE(isPlainName("a/b"));
}

TEST_CASE("Capsule: its folder name must be one plain name", "[editor][capsule][paths]") {
    const fs::path dir = workDir("capsule-name");
    CapsuleOptions options;
    options.destination = dir / "out";
    for (const char* bad : {"../escape", "a/b", "..", "c:\\x", "c:x"}) {
        options.name = bad;
        INFO(bad);
        const auto r = collectProject(ProjectState{}, dir / "p.wabcut", options);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    CHECK_FALSE(fs::exists(dir / "escape"));
}
