// Editor services over real files: import, library -> timeline drops, the project document (save, dirty state,
// autosave rotation, recovery), offline media and relinking, the playback session on the Null audio engine, and
// the export seam.

#include <wabcut/editor/Export.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/PlaybackSession.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/SourceEdit.h>

#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path testMedia(const char* name) {
    return fs::path(WABCUT_TEST_MEDIA_DIR) / name;
}

bool haveMedia() {
    return fs::exists(testMedia("h264_1080p60_aac.mp4")) && fs::exists(testMedia("sine_48k.wav"));
}

/// A fresh, empty folder under test-output/editor.
fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/// A project with one default sequence (1920x1080 30 fps, V1-V3, A1-A3).
ProjectState projectWithSequence(SequenceId* sequence) {
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    *sequence = created->id;
    return std::move(created->state);
}

MediaId importOne(EditSession& session, const fs::path& file, const fs::path& projectFile = {}) {
    const std::vector<fs::path> files{file};
    const MediaImportResult r = importMediaFiles(session, files, {.projectFile = projectFile});
    REQUIRE(r.failed.empty());
    REQUIRE(r.added.size() == 1);
    return r.added.front();
}

const Track& trackOf(const Sequence& seq, TrackKind kind, std::size_t index) {
    return *seq.tracks(kind)[index];
}

/// A.10: a game recording in memory (no file): 10 s of 30 fps video and one AAC stream per title ("" = untitled).
/// A 10 s sound file (one untitled stereo stream), without a file behind it.
MediaId addSound(ProjectState& state, const std::string& name) {
    MediaItem item;
    item.name = name;
    item.locator.path = "D:/sounds/" + name + ".wav";
    item.info.path = item.locator.path;
    item.info.container = "wav";
    item.info.duration = Ticks::fromSeconds(10);
    AudioStreamInfo a;
    a.index = 0;
    a.codec = "pcm_s16le";
    a.channels = 2;
    a.channelLayout = "stereo";
    a.duration = item.info.duration;
    item.info.audio.push_back(a);
    auto added = edit::addMedia(state, std::move(item));
    REQUIRE(added);
    state = std::move(added->state);
    return added->id;
}

MediaId addRecording(ProjectState& state, const std::string& name, std::initializer_list<const char*> titles) {
    MediaItem item;
    item.name = name;
    item.locator.path = "D:/recordings/" + name + ".mkv";
    item.info.path = item.locator.path;
    item.info.container = "matroska,webm";
    item.info.duration = Ticks::fromSeconds(10);
    VideoStreamInfo v;
    v.index = 0;
    v.codec = "h264";
    v.width = 1920;
    v.height = 1080;
    v.frameRate = kRate30;
    v.duration = item.info.duration;
    v.frameCount = 300;
    item.info.video.push_back(v);
    int index = 1;
    for (const char* title : titles) {
        AudioStreamInfo a;
        a.index = index++;
        a.codec = "aac";
        a.channels = 2;
        a.channelLayout = "stereo";
        a.duration = item.info.duration;
        a.title = title;
        item.info.audio.push_back(a);
    }
    auto added = edit::addMedia(state, std::move(item));
    REQUIRE(added);
    state = std::move(added->state);
    return added->id;
}

} // namespace

TEST_CASE("Import: media files become media items with locators, as one undo step", "[editor][import]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    const fs::path dir = workDir("import");
    EditSession session(ProjectState{});
    const std::vector<fs::path> files{testMedia("h264_1080p60_aac.mp4"), testMedia("sine_48k.wav"),
                                      dir / "missing.mp4", testMedia("h264_1080p60_aac.mp4")};
    const MediaImportResult r = importMediaFiles(session, files, {.projectFile = dir / "p.wabcut"});
    REQUIRE(r.added.size() == 2);
    REQUIRE(r.failed.size() == 1);
    CHECK(r.failed[0].file == dir / "missing.mp4");
    CHECK(r.failed[0].error.code == ErrorCode::NotFound);
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Import");

    const ProjectSnapshot snap = session.snapshot();
    const MediaItem* video = snap->findMedia(r.added[0]);
    REQUIRE(video);
    CHECK(video->name == "h264_1080p60_aac.mp4");
    CHECK(video->info.hasVideo());
    CHECK(video->info.hasAudio());
    CHECK(video->locator.fingerprint.isKnown());
    CHECK_FALSE(video->locator.offline);
    CHECK(pathFromUtf8(video->locator.path) == fs::absolute(testMedia("h264_1080p60_aac.mp4")).lexically_normal());
    CHECK(video->locator.relativePath == "../../media/h264_1080p60_aac.mp4");
    const MediaItem* audio = snap->findMedia(r.added[1]);
    REQUIRE(audio);
    CHECK_FALSE(audio->info.hasVideo());
    CHECK(audio->info.hasAudio());

    // Importing the same file again finds the existing item and adds nothing.
    const std::vector<fs::path> again{testMedia("h264_1080p60_aac.mp4")};
    const MediaImportResult r2 = importMediaFiles(session, again);
    CHECK(r2.added.empty());
    REQUIRE(r2.alreadyInProject.size() == 1);
    CHECK(r2.alreadyInProject[0] == r.added[0]);
    CHECK(session.undoCount() == 1);
}

TEST_CASE("Import: timeline files are routed to the interchange importer", "[editor][import]") {
    const fs::path fixtures(WABCUT_INTERCHANGE_FIXTURES);
    CHECK(classifyImportFile(fixtures / "avid_25fps.edl") == ImportFileKind::Interchange);
    CHECK(classifyImportFile(fixtures / "resolve_timeline.otio") == ImportFileKind::Interchange);
    CHECK(classifyImportFile(fixtures / "premiere_2997df.xml") == ImportFileKind::Interchange);
    CHECK(classifyImportFile("cut.wabcut") == ImportFileKind::Project);
    // P.2: caption files become caption tracks; an .xml only when it is Timed Text.
    CHECK(classifyImportFile(fixtures / "captions" / "basic-scc.scc") == ImportFileKind::Captions);
    CHECK(classifyImportFile(fixtures / "captions" / "basic-srt.srt") == ImportFileKind::Captions);
    CHECK(classifyImportFile(fixtures / "captions" / "basic-ttml.xml") == ImportFileKind::Captions);
    CHECK(classifyImportFile("missing.vtt") == ImportFileKind::Captions);
    if (haveMedia()) {
        CHECK(classifyImportFile(testMedia("h264_1080p60_aac.mp4")) == ImportFileKind::Media);
    }
    // 0.4: a folder is expanded by Import Folder; a timeline bundle folder stays a timeline (also without its content).
    const fs::path folders = workDir("classify-folders");
    fs::create_directories(folders / "Footage");
    fs::create_directories(folders / "cut.otiod");
    CHECK(classifyImportFile(folders / "Footage") == ImportFileKind::Folder);
    CHECK(classifyImportFile(folders / "cut.otiod") == ImportFileKind::Interchange);

    SequenceId existing;
    EditSession session(projectWithSequence(&existing));
    auto imported = importTimelineFile(session, fixtures / "avid_25fps.edl");
    REQUIRE(imported);
    CHECK_FALSE(imported->sequences.empty());
    CHECK_FALSE(imported->report.empty());
    CHECK(session.undoCount() == 1);
    // Added to the open project: the existing sequence is still there.
    const ProjectSnapshot snap = session.snapshot();
    CHECK(snap->findSequence(existing) != nullptr);
    CHECK(snap->sequences.size() == 1 + imported->sequences.size());

    CHECK_FALSE(importTimelineFile(session, fixtures / "malformed.edl"));
    CHECK(session.undoCount() == 1);
}

// ---- 0.4: Import Folder ----

namespace {

void writeText(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

/// A folder tree under `root`: each (relative path, test media file to copy, or nullptr for a small text file).
void makeTree(const fs::path& root, std::initializer_list<std::pair<const char*, const char*>> files) {
    for (const auto& [relative, source] : files) {
        const fs::path to = root / fs::path(relative);
        fs::create_directories(to.parent_path());
        if (source != nullptr) {
            fs::copy_file(testMedia(source), to, fs::copy_options::overwrite_existing);
        } else {
            writeText(to, "not media");
        }
    }
}

/// Imports `folder` as File > Import Folder does: collectImportFolder, then importMediaFiles into `target`.
MediaImportResult importFolderInto(EditSession& session, const fs::path& folder, BinId target) {
    const FolderImportPlan plan = collectImportFolder(folder);
    std::vector<fs::path> files;
    std::vector<std::vector<std::string>> bins;
    for (const FolderImportFile& f : plan.files) {
        files.push_back(f.file);
        bins.push_back(f.bins);
    }
    return importMediaFiles(session, files, {.bin = target, .binPaths = bins, .undoLabel = "Import Folder"});
}

std::optional<FolderSkip> skipReason(const FolderImportPlan& plan, const std::string& name) {
    for (const FolderImportSkipped& s : plan.skipped) {
        if (pathToUtf8(s.path.filename()) == name) {
            return s.reason;
        }
    }
    return std::nullopt;
}

const Bin* binNamed(const ProjectState& state, const std::string& name) {
    for (const Bin& b : state.bins) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Import Folder: subfolders become nested bins under the target bin in one undo step", "[editor][import][folder]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    const fs::path footage = workDir("import-folder") / "Footage";
    makeTree(footage, {{"a.mp4", "h264_1080p60_aac.mp4"},
                       {"Interviews/b.mp4", "h264_1080p60_aac.mp4"},
                       {"Interviews/Day2/c.wav", "sine_48k.wav"},
                       {"notes.txt", nullptr},
                       {".hidden.mp4", "h264_1080p60_aac.mp4"}});
    auto made = edit::addBin(ProjectState{}, "Target");
    REQUIRE(made);
    const BinId target = made->id;
    EditSession session(std::move(made->state));

    // The plan: a folder's own files (by name) before its subfolders', each with its bins.
    const FolderImportPlan plan = collectImportFolder(footage);
    REQUIRE(plan.files.size() == 3);
    CHECK(plan.files[0].file.filename() == "a.mp4");
    CHECK(plan.files[0].bins == std::vector<std::string>{"Footage"});
    CHECK(plan.files[1].file.filename() == "b.mp4");
    CHECK(plan.files[1].bins == std::vector<std::string>{"Footage", "Interviews"});
    CHECK(plan.files[2].file.filename() == "c.wav");
    CHECK(plan.files[2].bins == std::vector<std::string>{"Footage", "Interviews", "Day2"});
    CHECK(skipReason(plan, "notes.txt") == FolderSkip::NotMedia);
    CHECK(skipReason(plan, ".hidden.mp4") == FolderSkip::Hidden);
    CHECK_FALSE(plan.truncated);

    const MediaImportResult r = importFolderInto(session, footage, target);
    REQUIRE(r.failed.empty());
    REQUIRE(r.added.size() == 3);
    REQUIRE(r.createdBins.size() == 3);
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Import Folder");
    {
        const ProjectSnapshot snap = session.snapshot();
        const Bin* footageBin = snap->findBin(r.createdBins[0]);
        const Bin* interviews = snap->findBin(r.createdBins[1]);
        const Bin* day2 = snap->findBin(r.createdBins[2]);
        REQUIRE(footageBin);
        REQUIRE(interviews);
        REQUIRE(day2);
        CHECK(footageBin->name == "Footage");
        CHECK(footageBin->parent == target);
        CHECK(interviews->name == "Interviews");
        CHECK(interviews->parent == footageBin->id);
        CHECK(day2->name == "Day2");
        CHECK(day2->parent == interviews->id);
        CHECK(snap->findMedia(r.byFile[0])->bin == footageBin->id);
        CHECK(snap->findMedia(r.byFile[1])->bin == interviews->id);
        CHECK(snap->findMedia(r.byFile[2])->bin == day2->id);
    }
    // One undo removes every item and bin it made; redo brings them back.
    REQUIRE(session.undo());
    CHECK(session.snapshot()->media.empty());
    CHECK(session.snapshot()->bins.size() == 1);
    REQUIRE(session.redo());
    CHECK(session.snapshot()->media.size() == 3);
    CHECK(session.snapshot()->bins.size() == 4);
}

TEST_CASE("Import Folder: the same folder again adds nothing and makes no bins", "[editor][import][folder]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    const fs::path footage = workDir("import-folder-again") / "Footage";
    makeTree(footage, {{"a.mp4", "h264_1080p60_aac.mp4"},
                       {"Interviews/b.mp4", "h264_1080p60_aac.mp4"},
                       {"Interviews/Day2/c.wav", "sine_48k.wav"}});
    EditSession session(ProjectState{});
    const MediaImportResult first = importFolderInto(session, footage, BinId{});
    REQUIRE(first.added.size() == 3);
    const std::size_t bins = session.snapshot()->bins.size();
    CHECK(bins == 3);
    CHECK(session.snapshot()->findBin(first.createdBins[0])->parent == BinId{}); // the root

    const MediaImportResult again = importFolderInto(session, footage, BinId{});
    CHECK(again.added.empty());
    CHECK(again.alreadyInProject.size() == 3);
    CHECK(again.createdBins.empty());
    CHECK(session.snapshot()->bins.size() == bins);
    CHECK(session.undoCount() == 1);
}

TEST_CASE("Import Folder: a new file goes into the bin made for its folder before", "[editor][import][folder]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    const fs::path footage = workDir("import-folder-new-file") / "Footage";
    makeTree(footage, {{"a.mp4", "h264_1080p60_aac.mp4"}, {"Interviews/b.mp4", "h264_1080p60_aac.mp4"}});
    EditSession session(ProjectState{});
    REQUIRE(importFolderInto(session, footage, BinId{}).added.size() == 2);
    const Bin* interviews = binNamed(*session.snapshot(), "Interviews");
    REQUIRE(interviews);
    const BinId interviewsId = interviews->id;

    makeTree(footage, {{"Interviews/d.wav", "sine_48k.wav"}});
    const MediaImportResult r = importFolderInto(session, footage, BinId{});
    REQUIRE(r.added.size() == 1);
    CHECK(r.alreadyInProject.size() == 2);
    CHECK(r.createdBins.empty());
    CHECK(session.snapshot()->findMedia(r.added.front())->bin == interviewsId);
    CHECK(session.snapshot()->bins.size() == 2);
    CHECK(session.undoCount() == 2);
}

TEST_CASE("Import Folder: bundles and camera cards are left out and the file cap is reported", "[editor][import][folder]") {
    // Only names and folders matter here (nothing is probed): the files are small text files.
    const fs::path dir = workDir("import-folder-skips");
    const fs::path footage = dir / "Footage";
    makeTree(footage, {{"a.mp4", nullptr},
                       {"More/b.mp4", nullptr},
                       {"More/c.mov", nullptr},
                       {"x.otiod/content.otio", nullptr},
                       {"DCIM/PRIVATE/AVCHD/BDMV/STREAM/00000.MTS", nullptr}});

    const FolderImportPlan plan = collectImportFolder(footage);
    REQUIRE(plan.files.size() == 3);
    CHECK(plan.files[0].file.filename() == "a.mp4");
    CHECK(plan.files[1].bins == std::vector<std::string>{"Footage", "More"});
    CHECK(skipReason(plan, "x.otiod") == FolderSkip::TimelineBundle);
    CHECK(skipReason(plan, "DCIM") == FolderSkip::CameraCard);
    CHECK_FALSE(plan.truncated);

    // At most maxFiles files are looked at.
    const FolderImportPlan capped = collectImportFolder(footage, {.maxFiles = 2, .maxDepth = 8});
    CHECK(capped.files.size() == 2);
    CHECK(capped.truncated);

    // Folders deeper than maxDepth are left out (bundles and cards are named as such first).
    const FolderImportPlan shallow = collectImportFolder(footage, {.maxFiles = 1000, .maxDepth = 0});
    CHECK(shallow.files.size() == 1);
    CHECK(skipReason(shallow, "More") == FolderSkip::TooDeep);
    CHECK(skipReason(shallow, "DCIM") == FolderSkip::CameraCard);

    // A camera card chosen itself, or a folder inside one: all of it is left out.
    const FolderImportPlan card = collectImportFolder(footage / "DCIM");
    CHECK(card.files.empty());
    REQUIRE(card.skipped.size() == 1);
    CHECK(card.skipped.front().reason == FolderSkip::CameraCard);
    CHECK(collectImportFolder(footage / "DCIM" / "PRIVATE" / "AVCHD").files.empty());

    // Links are never followed (only when this system lets the test make one).
    std::error_code ec;
    fs::create_directory_symlink(footage / "More", footage / "Linked", ec);
    if (!ec) {
        const FolderImportPlan linked = collectImportFolder(footage);
        CHECK(skipReason(linked, "Linked") == FolderSkip::Link);
        CHECK(linked.files.size() == 3);
    }
}

#ifdef _WIN32
// kernel32, declared as the SDK does (no <windows.h> in this file).
extern "C" __declspec(dllimport) int __stdcall SetFileAttributesW(const wchar_t* lpFileName, unsigned long dwFileAttributes);
#endif

TEST_CASE("Import Folder: Windows hidden files are left out as hidden and not as not media", "[editor][import][folder]") {
#ifndef _WIN32
    SKIP("Windows file attributes");
#else
    // Fix round 1 (D): Explorer hides desktop.ini and Thumbs.db (hidden, often system too); the toast's "N files were
    // left out (not media)" must not count them.
    constexpr unsigned long kHidden = 0x2; // FILE_ATTRIBUTE_HIDDEN
    constexpr unsigned long kSystem = 0x4; // FILE_ATTRIBUTE_SYSTEM
    const fs::path dir = workDir("import-folder-hidden");
    const fs::path captures = dir / "Captures";
    makeTree(captures, {{"a.mp4", nullptr},
                        {"desktop.ini", nullptr},
                        {"Thumbs.db", nullptr},
                        {"notes.txt", nullptr},
                        {"Old/b.mp4", nullptr},
                        {"Session/c.mp4", nullptr}});
    REQUIRE(SetFileAttributesW((captures / "desktop.ini").c_str(), kHidden | kSystem) != 0);
    REQUIRE(SetFileAttributesW((captures / "Thumbs.db").c_str(), kHidden) != 0);
    REQUIRE(SetFileAttributesW((captures / "Old").c_str(), kHidden) != 0);
    // A customised folder carries the system attribute (its own desktop.ini sets it): Explorer shows it, so it is
    // looked in.
    REQUIRE(SetFileAttributesW((captures / "Session").c_str(), kSystem) != 0);

    const FolderImportPlan plan = collectImportFolder(captures);
    REQUIRE(plan.files.size() == 2);
    CHECK(plan.files[0].file.filename() == "a.mp4");
    CHECK(plan.files[1].file.filename() == "c.mp4");
    CHECK(skipReason(plan, "desktop.ini") == FolderSkip::Hidden);
    CHECK(skipReason(plan, "Thumbs.db") == FolderSkip::Hidden);
    CHECK(skipReason(plan, "Old") == FolderSkip::Hidden);
    CHECK(skipReason(plan, "notes.txt") == FolderSkip::NotMedia);
    CHECK(std::ranges::count_if(plan.skipped, [](const FolderImportSkipped& s) { return s.reason == FolderSkip::NotMedia; }) == 1);
#endif
}

TEST_CASE("Capture folders: Game Bar and ShadowPlay and OBS profiles that exist", "[editor][import][capture]") {
    const fs::path dir = workDir("capture-folders");
    const fs::path videos = dir / "Videos";
    fs::create_directories(videos / "Captures");
    fs::create_directories(videos / "NVIDIA");
    const fs::path recordings = dir / "rec";
    const fs::path advanced = dir / "adv";
    fs::create_directories(recordings);
    fs::create_directories(advanced);
    const auto withSlashes = [](const fs::path& p, char slash) {
        std::string text = pathToUtf8(p.lexically_normal());
        std::ranges::replace(text, slash == '/' ? '\\' : '/', slash);
        return text;
    };
    const fs::path obs = dir / "obs-studio";
    // Two profiles at one folder, written with / and with \ (and a trailing one): one entry, the first profile's name.
    writeText(obs / "basic" / "profiles" / "A" / "basic.ini",
              "\xEF\xBB\xBF[General]\nName=Streaming\n\n[Output]\nMode=Simple\n\n[SimpleOutput]\nFilePath=" +
                  withSlashes(recordings, '/') + "\n");
    writeText(obs / "basic" / "profiles" / "B" / "basic.ini",
              "[SimpleOutput]\r\nFilePath=" + withSlashes(recordings, '\\') + "\\\r\n");
    // Advanced output: its recording path first.
    writeText(obs / "basic" / "profiles" / "C" / "basic.ini",
              "[Output]\nMode=Advanced\n[SimpleOutput]\nFilePath=" + withSlashes(recordings, '/') +
                  "\n[AdvOut]\nRecFilePath=" + withSlashes(advanced, '/') + "\n");
    // A folder that is not there: no entry.
    writeText(obs / "basic" / "profiles" / "D" / "basic.ini",
              "[SimpleOutput]\nFilePath=" + withSlashes(dir / "missing", '/') + "\n");

    const std::vector<CaptureFolder> found = captureFolders(videos, obs);
    REQUIRE(found.size() == 4);
    CHECK(found[0].tool == CaptureTool::GameBar);
    CHECK(fs::equivalent(found[0].path, videos / "Captures"));
    CHECK(found[1].tool == CaptureTool::ShadowPlay);
    CHECK(fs::equivalent(found[1].path, videos / "NVIDIA"));
    CHECK(found[2].tool == CaptureTool::Obs);
    CHECK(found[2].profile == "Streaming");
    CHECK(fs::equivalent(found[2].path, recordings));
    CHECK(found[3].tool == CaptureTool::Obs);
    CHECK(found[3].profile == "C");
    CHECK(fs::equivalent(found[3].path, advanced));

    // Nothing there: nothing listed.
    CHECK(captureFolders(dir / "none", dir / "none").empty());
    CHECK(captureFolders({}, {}).empty());
}

TEST_CASE("Drop: library media lands on the tracks under the pointer, linked", "[editor][drop]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    SequenceId seqId;
    EditSession session(projectWithSequence(&seqId));
    const MediaId av = importOne(session, testMedia("h264_1080p60_aac.mp4"));
    const MediaId wav = importOne(session, testMedia("sine_48k.wav"));

    // Outside the tracks: V1 (targeted) and A1 (targeted), linked.
    auto first = dropMedia(session, {.media = av, .sequence = seqId, .at = Ticks{}});
    REQUIRE(first);
    REQUIRE(first->placed.size() == 2);
    {
        const SequencePtr seq = session.snapshot()->sequencePtr(seqId);
        const auto v = seq->findClip(first->placed[0]);
        const auto a = seq->findClip(first->placed[1]);
        REQUIRE(v);
        REQUIRE(a);
        CHECK(v->track->id == trackOf(*seq, TrackKind::Video, 0).id);
        CHECK(a->track->id == trackOf(*seq, TrackKind::Audio, 0).id);
        CHECK(v->clip().linkGroup);
        CHECK(v->clip().linkGroup == a->clip().linkGroup);
        CHECK(v->clip().duration() == Ticks::fromSeconds(10));
    }

    // On V2 (overwrite) at 2 s: video on V2, audio on A2 (the matching audio track: A1 has the first clip there).
    const TrackId v2 = trackOf(*session.snapshot()->sequencePtr(seqId), TrackKind::Video, 1).id;
    auto second =
        dropMedia(session, {.media = av, .sequence = seqId, .at = Ticks::fromSeconds(2), .track = v2,
                            .mode = DropMode::Overwrite});
    REQUIRE(second);
    {
        const SequencePtr seq = session.snapshot()->sequencePtr(seqId);
        const auto v = seq->findClip(second->placed[0]);
        const auto a = seq->findClip(second->placed[1]);
        REQUIRE(v);
        REQUIRE(a);
        CHECK(v->track->id == v2);
        CHECK(a->track->id == trackOf(*seq, TrackKind::Audio, 1).id);
        CHECK(v->clip().start() == Ticks::fromSeconds(2));
        // Overwrite moved nothing: the first clips still start at 0, whole.
        CHECK(seq->findClip(first->placed[0])->clip().start() == Ticks{});
        CHECK(seq->findClip(first->placed[0])->clip().duration() == Ticks::fromSeconds(10));
        CHECK(seq->findClip(first->placed[1])->clip().duration() == Ticks::fromSeconds(10));
    }

    // Audio-only media on A2 (Ctrl: insert at 0): only A2 gets a clip; sync-locked tracks make room.
    const TrackId a2 = trackOf(*session.snapshot()->sequencePtr(seqId), TrackKind::Audio, 1).id;
    auto third = dropMedia(session, {.media = wav, .sequence = seqId, .at = Ticks{}, .track = a2});
    REQUIRE(third);
    REQUIRE(third->placed.size() == 1);
    {
        const SequencePtr seq = session.snapshot()->sequencePtr(seqId);
        const auto a = seq->findClip(third->placed[0]);
        REQUIRE(a);
        CHECK(a->track->id == a2);
        const Ticks pushed = a->clip().duration();
        CHECK(seq->findClip(first->placed[0])->clip().start() == pushed);
    }
    CHECK(session.undoLabel() == "Insert");

    // Unknown media changes nothing.
    const std::uint64_t revision = session.revision();
    CHECK_FALSE(dropMedia(session, {.media = MediaId{999999}, .sequence = seqId, .at = Ticks{}}));
    CHECK(session.revision() == revision);
}

TEST_CASE("Drop: a plain drop overwrites only where it lands; the other kind avoids the clips on other tracks",
          "[editor][drop]") {
    SequenceId seqId;
    ProjectState state = projectWithSequence(&seqId); // V1-V3, A1-A3
    const MediaId below = addRecording(state, "Below", {""});
    const MediaId top = addRecording(state, "Top", {""});
    const MediaId music = addSound(state, "Music");
    EditSession session(state);
    const Sequence& fresh = *session.snapshot()->findSequence(seqId);
    const TrackId v1 = fresh.videoTracks[0]->id;
    const TrackId v2 = fresh.videoTracks[1]->id;
    const TrackId v3 = fresh.videoTracks[2]->id;
    const TrackId a1 = fresh.audioTracks[0]->id;
    const TrackId a2 = fresh.audioTracks[1]->id;
    const TrackId a3 = fresh.audioTracks[2]->id;
    const auto ranges = [&](TrackId track) {
        std::vector<std::pair<double, double>> out;
        for (const Clip& c : session.snapshot()->findSequence(seqId)->findTrack(track)->clips) {
            out.emplace_back(c.start().toSeconds(), c.end().toSeconds());
        }
        return out;
    };
    const auto trackOfClip = [&](ClipId clip) { return session.snapshot()->sequencePtr(seqId)->findClip(clip)->track->id; };
    using Ranges = std::vector<std::pair<double, double>>;

    // The layer below: an A/V clip on V1 and A1, 0-10 s.
    REQUIRE(dropMedia(session, {.media = below, .sequence = seqId, .at = Ticks{}, .track = v1, .mode = DropMode::Overwrite}));

    // The user's drop: another A/V clip onto V2 at 4 s. Its picture on V2, its sound on A2; V1 and A1 stay whole
    // (an insert would have cut them at 4 s and pushed their tails to 14 s).
    auto onV2 = dropMedia(session, {.media = top, .sequence = seqId, .at = Ticks::fromSeconds(4), .track = v2,
                                    .mode = DropMode::Overwrite});
    REQUIRE(onV2);
    REQUIRE(onV2->placed.size() == 2);
    CHECK(trackOfClip(onV2->placed[0]) == v2);
    CHECK(trackOfClip(onV2->placed[1]) == a2);
    CHECK(ranges(v1) == Ranges{{0, 10}});
    CHECK(ranges(a1) == Ranges{{0, 10}});
    CHECK(ranges(v2) == Ranges{{4, 14}});
    CHECK(ranges(a2) == Ranges{{4, 14}});
    CHECK(session.undoLabel() == "Overwrite");

    // Over the clip on V2 again (at 6 s): its own sound on A2 is what the drop replaces, so the new sound goes there too.
    auto replace = dropMedia(session, {.media = top, .sequence = seqId, .at = Ticks::fromSeconds(6), .track = v2,
                                       .mode = DropMode::Overwrite});
    REQUIRE(replace);
    CHECK(trackOfClip(replace->placed[1]) == a2);
    CHECK(ranges(v2) == Ranges{{4, 6}, {6, 16}});
    CHECK(ranges(a2) == Ranges{{4, 6}, {6, 16}});
    CHECK(ranges(a3).empty());

    // A3 holds music from 20 s; a drop on V3 there sends its sound to the nearest free track instead (A2, the
    // first one before A3; none comes after it): the music stays whole.
    REQUIRE(dropMedia(session, {.media = music, .sequence = seqId, .at = Ticks::fromSeconds(20), .track = a3,
                                .mode = DropMode::Overwrite}));
    auto onV3 = dropMedia(session, {.media = top, .sequence = seqId, .at = Ticks::fromSeconds(22), .track = v3,
                                    .mode = DropMode::Overwrite});
    REQUIRE(onV3);
    CHECK(trackOfClip(onV3->placed[0]) == v3);
    CHECK(trackOfClip(onV3->placed[1]) == a2); // A3 is taken there; A2 is the nearest free one before it
    CHECK(ranges(a3) == Ranges{{20, 30}});

    // On an audio track, the picture goes to the matching video track: a drop on A1 at 40 s puts it on V1.
    auto onA1 = dropMedia(session, {.media = top, .sequence = seqId, .at = Ticks::fromSeconds(40), .track = a1,
                                    .mode = DropMode::Overwrite});
    REQUIRE(onA1);
    CHECK(trackOfClip(onA1->placed[0]) == v1);
    CHECK(trackOfClip(onA1->placed[1]) == a1);

    // Without a free track the matching one is used, as Premiere patches: with music on A1 too, every audio track
    // has something at 24-34 s, so the sound of a drop on V2 goes to A2.
    REQUIRE(dropMedia(session, {.media = music, .sequence = seqId, .at = Ticks::fromSeconds(20), .track = a1,
                                .mode = DropMode::Overwrite}));
    auto busy = dropMedia(session, {.media = top, .sequence = seqId, .at = Ticks::fromSeconds(24), .track = v2,
                                    .mode = DropMode::Overwrite});
    REQUIRE(busy);
    CHECK(trackOfClip(busy->placed[1]) == a2);

    // Insert (Ctrl) keeps its meaning: it opens room on every sync-locked track.
    auto inserted = dropMedia(session, {.media = top, .sequence = seqId, .at = Ticks::fromSeconds(2), .track = v2,
                                        .mode = DropMode::Insert});
    REQUIRE(inserted);
    CHECK(ranges(v1) == Ranges{{0, 2}, {12, 20}, {50, 60}});
    CHECK(session.undoLabel() == "Insert");
}

TEST_CASE("Drop: a multi-track recording routes named streams by name and untitled ones from the pointer track",
          "[editor][drop][multitrack]") {
    SequenceId seqId;
    ProjectState state = projectWithSequence(&seqId); // A1-A3, empty and unnamed
    const MediaId shadowPlay = addRecording(state, "SP", {"", ""});
    const MediaId obs = addRecording(state, "OBS", {"Game", "Mic", "Discord", "Music"});

    // Untitled streams (ShadowPlay's game and mic) start at the track under the pointer, as before, and name the
    // free tracks they take.
    {
        const Sequence& fresh = *state.findSequence(seqId);
        const auto patches = patchesForDrop(fresh, *state.findMedia(shadowPlay), fresh.audioTracks[1]->id);
        REQUIRE(patches.size() == 3);
        CHECK(patches[0].source == StreamRef{StreamKind::Video, 0});
        CHECK(patches[1].source == StreamRef{StreamKind::Audio, 0});
        CHECK(patches[1].destination == fresh.audioTracks[1]->id);
        CHECK(patches[1].newTrack == "Game");
        CHECK(patches[2].destination == fresh.audioTracks[2]->id);
        CHECK(patches[2].newTrack == "Mic");
    }

    // Named streams find the track of their name wherever the drop lands: Mic goes to A1 although the drop is on A3.
    edit::TrackStateChange change;
    change.name = "Mic";
    auto renamed = edit::setTrackState(state, seqId, state.findSequence(seqId)->audioTracks[0]->id, change);
    REQUIRE(renamed);
    const ProjectState withMic = renamed->state;
    const Sequence& seq = *withMic.findSequence(seqId);
    const TrackId a1 = seq.audioTracks[0]->id;
    const TrackId a3 = seq.audioTracks[2]->id;
    const auto patches = patchesForDrop(seq, *withMic.findMedia(obs), a3);
    REQUIRE(patches.size() == 5);
    CHECK(patches[1].destination == a3); // Game takes the free track under the pointer
    CHECK(patches[1].newTrack == "Game");
    CHECK(patches[2].destination == a1); // Mic
    CHECK(patches[2].newTrack.empty());
    CHECK_FALSE(patches[3].destination); // Discord and Music: no free track from A3 on, so new ones
    CHECK(patches[3].newTrack == "Discord");
    CHECK_FALSE(patches[4].destination);
    CHECK(patches[4].newTrack == "Music");

    // The drop adds and names the tracks, as one undo step.
    EditSession session(withMic);
    auto dropped = dropMedia(session, {.media = obs, .sequence = seqId, .at = Ticks{}, .track = a3});
    REQUIRE(dropped);
    CHECK(dropped->placed.size() == 5);
    CHECK(dropped->createdTracks.size() == 2);
    {
        const SequencePtr after = session.snapshot()->sequencePtr(seqId);
        std::vector<std::string> names;
        for (const TrackPtr& t : after->audioTracks) {
            names.push_back(t->name);
        }
        CHECK(names == std::vector<std::string>{"Mic", "", "Game", "Discord", "Music"});
        CHECK(after->findClip(dropped->placed[2])->track->id == a1); // the recording's mic is on the Mic track
    }
    CHECK(session.undoCount() == 1);
    REQUIRE(session.undo());
    CHECK(session.snapshot()->findSequence(seqId)->audioTracks.size() == 3);
}

TEST_CASE("Drop: a sequence lands as a nested clip on the tracks under the pointer, never into itself",
          "[editor][drop][nested]") {
    SequenceId mainId;
    ProjectState state = projectWithSequence(&mainId); // V1-V3, A1-A3
    const MediaId recording = addRecording(state, "Rec", {""});
    auto second = edit::addSequence(state, edit::SequenceSettings{});
    REQUIRE(second);
    const SequenceId nestedId = second->id;
    EditSession session(second->state);
    // The sequence to nest holds the recording (picture and sound) on V1 and A1.
    REQUIRE(dropMedia(session, {.media = recording, .sequence = nestedId, .at = Ticks{}}));
    const Ticks length = session.snapshot()->findSequence(nestedId)->duration;
    REQUIRE(length > Ticks{});
    const Sequence& outer = *session.snapshot()->findSequence(mainId);
    const TrackId v2 = outer.videoTracks[1]->id;
    const TrackId a2 = outer.audioTracks[1]->id;

    // Dropped on V2 at 3 s: its picture on V2 and its mix on A2 (the matching track), linked, the whole sequence long.
    auto nested = dropMedia(session, {.sequence = mainId, .at = Ticks::fromSeconds(3), .track = v2,
                                      .mode = DropMode::Overwrite, .sourceSequence = nestedId});
    REQUIRE(nested);
    REQUIRE(nested->placed.size() == 2);
    const SequencePtr after = session.snapshot()->sequencePtr(mainId);
    const auto picture = after->findClip(nested->placed[0]);
    const auto sound = after->findClip(nested->placed[1]);
    REQUIRE(picture);
    REQUIRE(sound);
    CHECK(picture->track->id == v2);
    CHECK(sound->track->id == a2);
    CHECK(picture->clip().sequence == nestedId);
    CHECK(sound->clip().sequence == nestedId);
    CHECK(picture->clip().linkGroup);
    CHECK(picture->clip().linkGroup == sound->clip().linkGroup);
    CHECK(picture->clip().start() == Ticks::fromSeconds(3));
    CHECK(picture->clip().end() == Ticks::fromSeconds(3) + length);
    CHECK(session.undoLabel() == "Overwrite");

    // Never into itself, nor into a sequence it plays (that would loop): nothing changes.
    const std::size_t steps = session.undoCount();
    CHECK_FALSE(dropMedia(session, {.sequence = mainId, .at = Ticks{}, .track = v2, .sourceSequence = mainId}));
    CHECK_FALSE(dropMedia(session, {.sequence = nestedId, .at = Ticks{}, .sourceSequence = mainId}));
    CHECK_FALSE(dropMedia(session, {.sequence = mainId, .at = Ticks{}, .sourceSequence = SequenceId{999999}}));
    CHECK(session.undoCount() == steps);
}

TEST_CASE("Document: save, open, dirty state and Save As", "[editor][document]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    const fs::path dir = workDir("document");
    SequenceId seqId;
    EditSession session(projectWithSequence(&seqId));
    ProjectDocument doc(session);
    CHECK(doc.isUntitled());
    CHECK(doc.displayName() == "Untitled");
    CHECK_FALSE(doc.isDirty());

    const MediaId av = importOne(session, testMedia("h264_1080p60_aac.mp4"));
    CHECK(doc.isDirty());
    CHECK(doc.save().error().code == ErrorCode::InvalidArgument);

    const fs::path file = dir / "a.wabcut";
    REQUIRE(doc.saveAs(file));
    CHECK(fs::exists(file));
    CHECK_FALSE(doc.isDirty());
    CHECK(doc.displayName() == "a");

    REQUIRE(dropMedia(session, {.media = av, .sequence = seqId, .at = Ticks{}}));
    CHECK(doc.isDirty());
    REQUIRE(session.undo());
    CHECK_FALSE(doc.isDirty()); // back at the saved snapshot
    REQUIRE(session.redo());
    CHECK(doc.isDirty());
    REQUIRE(doc.save());
    CHECK_FALSE(doc.isDirty());
    CHECK(fs::exists(io::backupPath(file, 1))); // the previous save was rotated

    // Reopen in a new session: same sequence, same media.
    EditSession other;
    ProjectDocument reopened(other);
    REQUIRE(reopened.open(file));
    CHECK_FALSE(reopened.isDirty());
    CHECK(reopened.file() == fs::absolute(file).lexically_normal());
    const ProjectSnapshot a = session.snapshot();
    const ProjectSnapshot b = other.snapshot();
    REQUIRE(a->sequences.size() == b->sequences.size());
    CHECK(*a->sequences[0] == *b->sequences[0]);
    REQUIRE(b->media.size() == 1);
    CHECK(b->media[0]->locator.path == a->media[0]->locator.path);
    CHECK(b->media[0]->locator.fingerprint == a->media[0]->locator.fingerprint);
    CHECK(b->media[0]->info == a->media[0]->info);
    CHECK(reopened.offlineMedia().empty());

    // Save As into a subfolder re-bases the relative media paths on the new location.
    const fs::path moved = dir / "sub" / "deeper" / "b.wabcut";
    fs::create_directories(moved.parent_path());
    REQUIRE(reopened.saveAs(moved));
    auto loaded = io::loadProject(moved);
    REQUIRE(loaded);
    CHECK(loaded->media[0]->locator.relativePath == "../../../../media/h264_1080p60_aac.mp4");
}

// UX rework: File > Save a Copy writes the project somewhere else and leaves the document as it was (its file, its
// unsaved changes, what it autosaves); the copy's relative media paths are for its own folder.
TEST_CASE("Document: Save a Copy writes another file and leaves the document as it was", "[editor][document]") {
    const fs::path dir = workDir("save-copy");
    SequenceId seqId;
    ProjectState initial = projectWithSequence(&seqId);
    const MediaId voice = addSound(initial, "voice");
    for (MediaPtr& media : initial.media) { // the sound file in the work folder (it need not exist)
        if (media->id == voice) {
            auto item = std::make_shared<MediaItem>(*media);
            item->locator.path = pathToUtf8(dir / "media" / "voice.wav");
            item->info.path = item->locator.path;
            media = std::move(item);
        }
    }
    EditSession session(std::move(initial));
    ProjectDocument doc(session, {.saveBackups = 2});
    const fs::path file = dir / "film.wabcut";
    REQUIRE(doc.saveAs(file));
    REQUIRE(session.apply("New Sequence",
                          [](const ProjectState& s) { return edit::addSequence(s, edit::SequenceSettings{}); }));
    REQUIRE(doc.isDirty());
    const auto autosaveBefore = doc.prepareAutosave().has_value();

    const fs::path copy = dir / "copies" / "deeper" / "film copy.wabcut";
    fs::create_directories(copy.parent_path());
    REQUIRE(doc.saveCopy(copy));
    auto copied = io::loadProject(copy);
    REQUIRE(copied);
    CHECK(copied->sequences.size() == 2); // the project as it is now
    REQUIRE(copied->media.size() == 1);
    CHECK(copied->media[0]->locator.relativePath == "../../media/voice.wav"); // from the copy's folder
    // The document is as it was: same file, still unsaved, still to autosave; the project file has one sequence.
    CHECK(doc.file() == fs::absolute(file).lexically_normal());
    CHECK(doc.isDirty());
    CHECK(doc.prepareAutosave().has_value() == autosaveBefore);
    auto original = io::loadProject(file);
    REQUIRE(original);
    CHECK(original->sequences.size() == 1);
    // A copy rotates no backups next to itself, even over an earlier copy.
    REQUIRE(doc.saveCopy(copy));
    CHECK_FALSE(fs::exists(io::backupPath(copy, 1)));
}

TEST_CASE("Document: autosave rotates recovery copies, recovery reopens them", "[editor][document]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    const fs::path dir = workDir("autosave");
    SequenceId seqId;
    EditSession session(projectWithSequence(&seqId));
    ProjectDocument doc(session, {.autosaveCopies = 3});
    const MediaId av = importOne(session, testMedia("h264_1080p60_aac.mp4"));
    const fs::path file = dir / "auto.wabcut";
    REQUIRE(doc.saveAs(file));

    auto autosave = [&] {
        auto written = doc.autosave();
        REQUIRE(written);
        return *written;
    };
    CHECK_FALSE(autosave()); // nothing changed since the save
    for (int i = 0; i < 4; ++i) {
        REQUIRE(dropMedia(session, {.media = av, .sequence = seqId, .at = Ticks::fromSeconds(i)}));
        CHECK(autosave());
        CHECK_FALSE(autosave());
    }
    CHECK(fs::exists(io::autosavePath(file)));
    fs::path second = io::autosavePath(file);
    second += "2";
    fs::path third = io::autosavePath(file);
    third += "3";
    fs::path fourth = io::autosavePath(file);
    fourth += "4";
    CHECK(fs::exists(second));
    CHECK(fs::exists(third));
    CHECK_FALSE(fs::exists(fourth));

    const std::vector<RecoveryFile> recovery = doc.recoveryFiles(file);
    int autosaves = 0;
    for (const RecoveryFile& r : recovery) {
        CHECK(r.loadable);
        autosaves += r.kind == RecoveryFile::Kind::Autosave ? 1 : 0;
    }
    CHECK(autosaves == 3);
    for (std::size_t i = 1; i < recovery.size(); ++i) {
        CHECK(recovery[i - 1].modified >= recovery[i].modified);
    }

    // The newest autosave holds four drops; the second newest three.
    EditSession other;
    ProjectDocument recovered(other);
    REQUIRE(recovered.recover(second, file));
    CHECK(recovered.isDirty());
    CHECK(recovered.file() == fs::absolute(file).lexically_normal());
    const auto countClips = [](const EditSession& s) {
        std::size_t n = 0;
        s.snapshot()->sequences[0]->forEachTrack([&](const Track& t) { n += t.clips.size(); });
        return n;
    };
    REQUIRE(recovered.recover(io::autosavePath(file), file));
    const std::size_t newest = countClips(other);
    REQUIRE(recovered.recover(second, file));
    CHECK(countClips(other) < newest);

    // Untitled projects autosave into the configured folder.
    EditSession untitledSession(projectWithSequence(&seqId));
    ProjectDocument untitled(untitledSession, {.untitledAutosaveDir = dir / "untitled"});
    importOne(untitledSession, testMedia("sine_48k.wav"));
    REQUIRE(untitled.autosave());
    CHECK(fs::exists(untitled.autosaveFile()));
    CHECK(untitled.autosaveFile().parent_path() == dir / "untitled");
}

TEST_CASE("Document: moved media opens offline and relinks by name and fingerprint", "[editor][relink]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    const fs::path dir = workDir("relink");
    fs::create_directories(dir / "footage");
    fs::copy_file(testMedia("h264_1080p60_aac.mp4"), dir / "footage" / "clip.mp4");
    const fs::path file = dir / "project" / "p.wabcut";
    fs::create_directories(file.parent_path());
    MediaId id;
    {
        SequenceId seqId;
        EditSession session(projectWithSequence(&seqId));
        ProjectDocument doc(session);
        id = importOne(session, dir / "footage" / "clip.mp4");
        REQUIRE(dropMedia(session, {.media = id, .sequence = seqId, .at = Ticks{}}));
        REQUIRE(doc.saveAs(file));
    }
    fs::create_directories(dir / "moved" / "deeper");
    fs::rename(dir / "footage" / "clip.mp4", dir / "moved" / "deeper" / "clip.mp4");
    // A different file with the same name elsewhere is not the clip.
    fs::create_directories(dir / "impostor");
    fs::copy_file(testMedia("sine_48k.wav"), dir / "impostor" / "clip.mp4");

    EditSession session;
    ProjectDocument doc(session);
    REQUIRE(doc.open(file));
    CHECK_FALSE(doc.isDirty());
    REQUIRE(doc.offlineMedia() == std::vector<MediaId>{id});
    CHECK(session.snapshot()->findMedia(id)->locator.offline);

    const auto candidates = doc.findRelinkCandidates(id, {dir / "impostor", dir / "moved"});
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].fingerprintMatches); // matches first
    CHECK(candidates[0].file == fs::absolute(dir / "moved" / "deeper" / "clip.mp4").lexically_normal());
    CHECK_FALSE(candidates[1].fingerprintMatches);

    const auto refused = doc.relink(id, candidates[1].file);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    REQUIRE(doc.relink(id, candidates[0].file));
    CHECK(doc.offlineMedia().empty());
    CHECK(pathFromUtf8(session.snapshot()->findMedia(id)->locator.path) == candidates[0].file);
    CHECK(doc.isDirty());
    CHECK(session.undoLabel() == "Relink Media");

    // relinkAll() after another move; then a project moved together with its media relinks by itself.
    EditSession session2;
    ProjectDocument doc2(session2);
    REQUIRE(doc2.open(file));
    REQUIRE(doc2.offlineMedia().size() == 1);
    const auto relinked = doc2.relinkAll({dir});
    REQUIRE(relinked);
    CHECK(*relinked == std::vector<MediaId>{id});
    CHECK(doc2.offlineMedia().empty());
    REQUIRE(doc2.saveAs(dir / "moved" / "p.wabcut")); // next to the media: relative path "deeper/clip.mp4"

    const fs::path elsewhere = workDir("relink-moved");
    fs::rename(dir / "moved", elsewhere / "moved"); // project and media move together
    EditSession session3;
    ProjectDocument doc3(session3);
    REQUIRE(doc3.open(elsewhere / "moved" / "p.wabcut"));
    CHECK(doc3.offlineMedia().empty());
    CHECK(pathFromUtf8(session3.snapshot()->findMedia(id)->locator.path) ==
          fs::absolute(elsewhere / "moved" / "deeper" / "clip.mp4").lexically_normal());
}

TEST_CASE("Playback session: the transport drives timeline audio on the Null engine", "[editor][playback]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    SequenceId seqId;
    EditSession session(projectWithSequence(&seqId));
    const MediaId av = importOne(session, testMedia("h264_1080p60_aac.mp4"));
    REQUIRE(dropMedia(session, {.media = av, .sequence = seqId, .at = Ticks{}}));

    PlaybackSessionOptions options;
    options.audio.backend = audio::AudioBackend::Null;
    options.audio.declickTime = Ticks{};
    PlaybackSession playback(session, options);
    REQUIRE(playback.start());
    REQUIRE(playback.hasSound());
    playback.setSequence(seqId);
    CHECK(playback.stats().video.transport == "timeline audio (Null)");

    // Real-time pulls of 10 ms blocks, as the device thread would.
    std::vector<float> block(480 * 2);
    double energy = 0.0;
    auto pullFor = [&](int milliseconds) {
        for (int i = 0; i < milliseconds / 10; ++i) {
            REQUIRE(playback.engine().pull(block));
            for (float s : block) {
                energy += std::abs(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    pullFor(200);
    CHECK(energy == 0.0); // paused
    playback.play();
    CHECK(playback.isPlaying());
    pullFor(1000);
    const Ticks played = playback.position();
    CHECK(played > Ticks{kTicksPerSecond * 6 / 10});
    CHECK(played < Ticks{kTicksPerSecond * 14 / 10});
    CHECK(energy > 100.0); // the clip's tones were heard
    const PlaybackSessionStats stats = playback.stats();
    CHECK(stats.audio.framesFed > 0);
    CHECK(stats.audio.feedErrors == 0);
    CHECK(stats.video.published > 0);
    CHECK(stats.audioBackend == "Null");

    playback.pause();
    playback.seek(Ticks::fromSeconds(2));
    pullFor(200);
    CHECK(playback.position() == Ticks::fromSeconds(2));
    playback.stepFrames(1);
    pullFor(50);
    CHECK(playback.position() == Ticks::fromSeconds(2) + framesToTicks(1, kRate30, Rounding::Floor).value());
    playback.stop();
    CHECK_FALSE(playback.hasSound());
}

TEST_CASE("Export seam: unavailable until an exporter registers", "[editor][export]") {
    SequenceId seqId;
    EditSession session(projectWithSequence(&seqId));
    REQUIRE(session.apply("Duration", [&](const ProjectState& state) {
        return edit::setSequenceDuration(state, seqId, Ticks::fromSeconds(1));
    }));

    setExporter(nullptr);
    const auto none = exportSequence(session, {.sequence = seqId, .output = "out.mp4"});
    REQUIRE_FALSE(none);
    CHECK(none.error().code == ErrorCode::Unsupported);

    struct FakeExporter final : Exporter {
        ExportRequest seen;
        std::string name() const override { return "fake"; }
        std::expected<ExportResult, Error> run(std::shared_ptr<const ProjectState>, const ExportRequest& request,
                                               const ExportProgress& progress) override {
            seen = request;
            if (progress) {
                progress(1.0);
            }
            return ExportResult{request.output, 30, 48000};
        }
    };
    auto fake = std::make_shared<FakeExporter>();
    setExporter(fake);
    const auto done = exportSequence(session, {.sequence = seqId, .output = "out.mp4"});
    setExporter(nullptr);
    REQUIRE(done);
    CHECK(done->videoFrames == 30);
    REQUIRE(fake->seen.range);
    CHECK(fake->seen.range->start == Ticks{});
    CHECK(fake->seen.range->duration == Ticks::fromSeconds(1));
    CHECK(exportSequence(session, {.sequence = SequenceId{424242}}).error().code == ErrorCode::Unsupported);
}

TEST_CASE("Offline checks and relinking skip generated items (titles)", "[editor][relink][titles]") {
    SequenceId seq;
    const ProjectState base = projectWithSequence(&seq);
    const TrackId v1 = base.findSequence(seq)->videoTracks[0]->id;
    const auto titled = edit::addTitle(base, seq, v1, Ticks{}, Ticks::fromSeconds(2), "Opening");
    REQUIRE(titled);
    const MediaId title = titled->createdMedia;
    const fs::path project = workDir("titles") / "titles.wabcut";

    const auto [checked, reason] = checkMediaOnDisk(*titled->state.findMedia(title), project);
    CHECK_FALSE(checked.locator.offline);
    CHECK(reason.empty());
    int changed = -1;
    const ProjectState after = withMediaChecked(titled->state, project, &changed);
    CHECK(changed == 0);
    CHECK(after == titled->state);

    EditSession session(titled->state);
    ProjectDocument doc(session);
    CHECK(doc.offlineMedia().empty());
    CHECK(doc.findRelinkCandidates(title, {workDir("titles-search")}).empty());
    CHECK(doc.relink(title, project).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Drop: real OBS MKV and hybrid MP4 and ShadowPlay recordings land on named linked tracks (A.10)",
          "[editor][drop][multitrack][media]") {
    const fs::path obs = testMedia("obs_multitrack_titled.mkv");
    const fs::path hybrid = testMedia("obs_hybrid_titled.mp4");
    const fs::path shadowPlay = testMedia("shadowplay_two_track.mp4");
    if (!fs::exists(obs) || !fs::exists(hybrid) || !fs::exists(shadowPlay)) {
        SKIP("multi-track test media missing");
    }
    SequenceId seqId;
    EditSession session(projectWithSequence(&seqId)); // A1-A3, empty and unnamed
    const MediaId recordings[] = {importOne(session, obs), importOne(session, hybrid), importOne(session, shadowPlay)};
    // Each recording after the last, dropped outside the tracks: the probe's stream names pick the tracks.
    std::vector<std::vector<std::string>> heardOn;
    for (const MediaId media : recordings) {
        const Ticks at = session.snapshot()->findSequence(seqId)->contentEnd();
        auto dropped = dropMedia(session, {.media = media, .sequence = seqId, .at = at, .track = TrackId{}});
        REQUIRE(dropped);
        const Sequence& seq = *session.snapshot()->findSequence(seqId);
        std::vector<std::string> names;
        LinkGroupId group;
        for (const ClipId id : dropped->placed) {
            const auto loc = seq.findClip(id);
            REQUIRE(loc);
            CHECK(loc->clip().linkGroup.isValid()); // one linked clip per stream, all in one group
            if (!group.isValid()) {
                group = loc->clip().linkGroup;
            }
            CHECK(loc->clip().linkGroup == group);
            if (loc->track->kind == TrackKind::Audio) {
                names.push_back(loc->track->name);
            }
        }
        heardOn.push_back(std::move(names));
    }
    CHECK(heardOn[0] == std::vector<std::string>{"Game", "Mic", "Discord", "Music"}); // MKV track titles
    CHECK(heardOn[1] == std::vector<std::string>{"Game", "Mic", "Discord"});          // MP4 handler names
    CHECK(heardOn[2] == std::vector<std::string>{"Game", "Mic"});                     // untitled pair
    // Three recordings, one Mic track (muting it mutes every mic), and only the Music track was added.
    const Sequence& seq = *session.snapshot()->findSequence(seqId);
    REQUIRE(seq.audioTracks.size() == 4);
    CHECK(seq.audioTracks[1]->name == "Mic");
    // Every recording's mic is on it (the first one's audio runs 21 ms past its last video frame, so the next drop, on
    // the frame grid, inserts before that tail and splits it off: a fourth clip of the same media).
    std::vector<MediaId> mics;
    for (const Clip& c : seq.audioTracks[1]->clips) {
        if (std::ranges::find(mics, c.media) == mics.end()) {
            mics.push_back(c.media);
        }
    }
    CHECK(mics.size() == 3);
    CHECK(seq.audioTracks[1]->role == TrackRole::Dialogue);
    CHECK(seq.audioTracks[3]->name == "Music");
}

TEST_CASE("Playback session: playback starting over a scrub window fades it out at once", "[editor][playback][scrub]") {
    if (!haveMedia()) {
        SKIP("test media missing");
    }
    SequenceId seqId;
    EditSession session(projectWithSequence(&seqId));
    const MediaId av = importOne(session, testMedia("h264_1080p60_aac.mp4"));
    REQUIRE(dropMedia(session, {.media = av, .sequence = seqId, .at = Ticks{}}));

    PlaybackSessionOptions options;
    options.audio.backend = audio::AudioBackend::Null;
    options.audio.declickTime = Ticks{};
    PlaybackSession playback(session, options);
    REQUIRE(playback.start());
    playback.setSequence(seqId);
    playback.seek(Ticks::fromSeconds(1));
    std::vector<float> block(480 * 2);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(playback.engine().pull(block));
    }
    playback::AudioScrubber* scrubber = playback.audioScrubber();
    REQUIRE(scrubber != nullptr);

    // A scrub window (its worker renders it), and the device starts playing it.
    REQUIRE(playback.scrubAudio(Ticks::fromSeconds(2)));
    for (int i = 0; i < 1000 && scrubber->stats().rendered == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(scrubber->stats().rendered == 1);
    REQUIRE(playback.engine().pull(block)); // 10 ms of the 60 ms window
    REQUIRE(scrubber->stats().started == 1);

    // Space: playback starts and the window fades out at once (the rest of it is not heard over the program).
    playback.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // the worker hands the stop over (no device time passes)
    for (int i = 0; i < 200 && scrubber->stats().cutShort == 0; ++i) {
        REQUIRE(playback.engine().pull(block));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(scrubber->stats().cutShort == 1);
    CHECK(scrubber->stats().framesPlayed < 60 * 48); // cut short
    // While playing nothing scrubs.
    CHECK_FALSE(playback.scrubAudio(Ticks::fromSeconds(3)));
    playback.pause();
    playback.stop();
}
