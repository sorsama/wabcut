// .wabcut JSON: lossless round trip, schema migrations, domain checks, atomic saves with rotated recovery
// copies, read-only loading, crash recovery and autosave.

#include "TimelineFixture.h"

#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/ColorManagementOps.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/FieldOptionsOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/SubmixOps.h>
#include <wabcut/model/Synthetic.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

/// The schemaVersion line every file is written with.
const std::string kCurrentSchema = "\"schemaVersion\": " + std::to_string(io::kSchemaVersion);

/// A project that exercises every field the file format stores.
ProjectState richProject() {
    Timeline tl(R"(
        V2 lock   |....Cccccc|
        V1 tgt    |AaaaB3bb..|
        A1 tgt dialogue |AaaaB3bb..|
        A2 nosync music |Mmmmmmmmmm|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    // The master audio bus: a true-peak limiter with an animated ceiling, and a track EQ on A2.
    s = ok(edit::addEffect(s, tl.seq, edit::EffectTarget::masterAudio(), "wabcut.audio.limiter",
                           {{"ceiling", ParamValue(Curve{{Keyframe{f(0), -1.0}, Keyframe{f(5), -2.0}}})}}))
            .state;
    s = ok(edit::addEffect(s, tl.seq, edit::EffectTarget::ofTrack(tl.track("A2")), "wabcut.audio.eq3",
                           {{"lowGain", ParamValue(-6.0)}}))
            .state;

    auto bin = edit::addBin(s, "Footage – 日本語");
    REQUIRE(bin);
    s = bin->state;
    MediaItem item;
    item.bin = bin->id;
    item.name = "Interview 🎤";
    item.locator.path = "D:/プロジェクト/interview.mov";
    item.locator.relativePath = "../プロジェクト/interview.mov";
    item.locator.fingerprint = {123'456'789'012, 1'760'000'000'123'456'789, "0123456789abcdef"};
    item.locator.offline = true;
    item.locator.proxy = ProxyMedia{"D:/proxies/interview_proxy.mov",
                                    {"wabcut-proxy 1", "prores-proxy 960x540", {42, 7, "fedcba9876543210"}, 3}};
    item.info.path = item.locator.path;
    item.info.container = "mov,mp4,m4a,3gp,3g2,mj2";
    item.info.duration = Ticks{kTicksPerSecond * 3600 * 10 + 1}; // 10 h and one tick
    item.info.fileSize = 123'456'789'012;
    item.info.modifiedTime = 1'760'000'000;
    VideoStreamInfo v;
    v.index = 0;
    v.codec = "hevc";
    v.profile = "Main 10";
    v.width = 3840;
    v.height = 2160;
    v.frameRate = kRate29_97;
    v.variableFrameRate = true;
    v.sampleAspect = {4, 3};
    v.pixelFormat = "yuv420p10le";
    v.bitDepth = 10;
    v.chromaSubsampling = "4:2:0";
    v.color = {ColorMatrix::BT2020NonConstant, ColorRange::Limited, ColorPrimaries::BT2020, TransferFunction::PQ,
               ChromaLocation::TopLeft};
    v.alpha = AlphaMode::Premultiplied;
    v.rotationDegrees = 90;
    v.duration = item.info.duration;
    v.frameCount = 1'078'920;
    item.info.video.push_back(v);
    AudioStreamInfo a;
    a.index = 1;
    a.codec = "pcm_s24le";
    a.sampleRate = {96000, 1};
    a.channels = 6;
    a.channelLayout = "5.1";
    a.duration = item.info.duration;
    item.info.audio.push_back(a);
    auto media = edit::addMedia(s, item);
    REQUIRE(media);
    s = media->state;

    // Marks, markers, a trailing gap, a work area and parameters that do not round-trip through float.
    s = ok(edit::markIn(s, tl.seq, f(1))).state;
    s = ok(edit::markOut(s, tl.seq, f(7))).state;
    s = ok(edit::addMarker(s, tl.seq, {f(3), f(2), "Chapter 1", "multi\nline \"comment\"", 4})).state;
    s = ok(edit::setSequenceDuration(s, tl.seq, f(14))).state;
    s = ok(edit::setWorkArea(s, tl.seq, TimeRange{f(2), f(9)})).state;
    ClipProperties props;
    props.set(ParamId::PositionX, 0.1);
    props.set(ParamId::PositionY, -1.0 / 3.0);
    props.set(ParamId::Scale, 1.2345678901234567);
    props.set(ParamId::Rotation, -720.25);
    props.set(ParamId::AnchorX, 1e-300);
    props.set(ParamId::AnchorY, 4096.5);
    props.set(ParamId::Opacity, 0.3);
    props.set(ParamId::VolumeDb, -96.0);
    props.set(ParamId::Pan, -1.0);
    edit::ClipAttributeChange change;
    change.properties = props;
    change.name = "B (alt take)";
    change.colorIndex = 7;
    change.enabled = false;
    s = ok(edit::setClipAttributes(s, tl.seq, std::vector<ClipId>{clipAt(s, tl.seq, "V1@4")}, change)).state;

    // A cross dissolve on the V1 edit point (B has 3 frames of head handle, A has plenty of tail).
    {
        auto seq = std::make_shared<Sequence>(*s.findSequence(tl.seq));
        auto v1 = std::make_shared<Track>(*seq->videoTracks[0]);
        v1->transitions.push_back(Transition{s.ids.allocate<TransitionId>(), clipAt(s, tl.seq, "V1@0"),
                                             clipAt(s, tl.seq, "V1@4"), f(2), f(1), "crossDissolve"});
        seq->videoTracks[0] = v1;
        const IdGenerator ids = s.ids;
        s = s.withSequence(seq);
        s.ids = ids;
    }

    // A second sequence in the bin, with a long clip of the new media far down the timeline.
    edit::SequenceSettings settings;
    settings.name = "4K DF";
    settings.frameRate = kRate29_97;
    settings.dropFrame = true;
    settings.width = 3840;
    settings.height = 2160;
    settings.audioRate = {96000, 1};
    settings.videoTracks = 1;
    settings.audioTracks = 1;
    settings.bin = bin->id;
    auto second = edit::addSequence(s, settings);
    REQUIRE(second);
    s = second->state;
    const Sequence& seq2 = *s.findSequence(second->id);
    edit::SourceClip src{media->id, {Ticks{1}, Ticks{kTicksPerSecond * 3600}}, edit::defaultSourcePatches(seq2, *s.findMedia(media->id))};
    s = ok(edit::overwriteEdit(s, second->id, src, Ticks{kTicksPerSecond * 3600 * 5 + 7})).state;
    requireValid(s);
    return s;
}

fs::path testDir(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "model-io" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void writeFile(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string replaceOnce(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    INFO("replacing " << from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

/// Name -> (size, mtime, bytes) of every file in a directory.
std::map<std::string, std::string> snapshotDirectory(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const auto time = entry.last_write_time().time_since_epoch().count();
        out[pathToUtf8(entry.path().filename())] = std::to_string(entry.file_size()) + "@" + std::to_string(static_cast<long long>(time)) + ":" +
                                                   readFile(entry.path());
    }
    return out;
}

ErrorCode loadError(const std::string& text) {
    const auto r = io::fromJson(text);
    INFO((r ? std::string("loaded") : r.error().message));
    REQUIRE_FALSE(r);
    return r.error().code;
}

/// A project as M2 (schema 1) wrote it.
constexpr const char* kSchemaV1 = R"({
  "format": "wabcut-project",
  "schemaVersion": 1,
  "nextId": 20,
  "bins": [],
  "media": [
    {"id": 1, "bin": 0, "name": "A", "path": "D:/footage/A.mov", "proxyPath": "D:/proxies/A.mov",
     "info": {"path": "D:/footage/A.mov", "container": "mov", "duration": 254016000000, "fileSize": 10,
              "modifiedTime": 5, "isStillImage": false,
              "video": [{"index": 0, "codec": "h264", "width": 1920, "height": 1080,
                         "frameRate": {"num": 25, "den": 1}, "duration": 254016000000,
                         "color": {"matrix": "bt709", "range": "limited", "primaries": "bt709", "transfer": "bt709"}}],
              "audio": [{"index": 1, "codec": "aac", "sampleRate": {"num": 48000, "den": 1}, "channels": 2,
                         "channelLayout": "stereo", "duration": 254016000000}]}}
  ],
  "sequences": [
    {"id": 2, "bin": 0, "name": "Sequence 01", "frameRate": {"num": 25, "den": 1}, "width": 1920, "height": 1080,
     "audioRate": {"num": 48000, "den": 1}, "dropFrame": false, "markIn": null, "markOut": null, "markers": [],
     "videoTracks": [
       {"id": 3, "name": "", "enabled": true, "locked": false, "syncLocked": true, "muted": false, "solo": false,
        "targeted": true,
        "clips": [{"id": 10, "media": 1, "stream": {"kind": "video", "index": 0}, "start": 0,
                   "duration": 101606400000, "sourceIn": 0, "speed": {"num": 1, "den": 1}, "enabled": true,
                   "linkGroup": 0, "name": "A", "colorIndex": 2,
                   "properties": {"position": {"value": [10.0, -20.5]}, "scale": {"value": 1.0},
                                  "rotation": {"value": 0.0}, "anchor": {"value": [0.0, 0.0]},
                                  "opacity": {"value": 0.5}, "volumeDb": {"value": 0.0}, "pan": {"value": 0.0}}}]}],
     "audioTracks": [
       {"id": 4, "name": "", "enabled": true, "locked": false, "syncLocked": true, "muted": false, "solo": false,
        "targeted": true,
        "clips": [{"id": 11, "media": 1, "stream": {"kind": "audio", "index": 0}, "start": 50803200000,
                   "duration": 152409600000, "sourceIn": 0, "speed": {"num": 1, "den": 1}, "enabled": true,
                   "linkGroup": 0, "name": "A", "colorIndex": 0,
                   "properties": {"volumeDb": {"value": -6.0}, "pan": {"value": 0.25}}}]}]}
  ]
})";

} // namespace

TEST_CASE("A project survives a JSON round trip exactly", "[model][io]") {
    const ProjectState original = richProject();
    const auto json = io::toJson(original);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == original);
    requireValid(*loaded);

    // Writing the loaded project again gives byte-identical output.
    const auto again = io::toJson(*loaded);
    REQUIRE(again);
    CHECK(*again == *json);
}

TEST_CASE("Ticks are written as exact integers in the current schema", "[model][io]") {
    const auto json = io::toJson(richProject());
    REQUIRE(json);
    CHECK(json->find(kCurrentSchema) != std::string::npos);
    CHECK(json->find("\"format\": \"wabcut-project\"") != std::string::npos);
    // 10 hours and one tick: far beyond what a double holds exactly at tick resolution.
    CHECK(json->find(std::to_string(kTicksPerSecond * 3600 * 10 + 1)) != std::string::npos);
    CHECK(json->find("\"start\": " + std::to_string(kTicksPerSecond * 3600 * 5 + 7)) != std::string::npos);
    // Parameters are keyed by their stable ids; defaults are not written.
    CHECK(json->find("\"opacity.opacity\"") != std::string::npos);
    CHECK(json->find("\"sampleHash\": \"0123456789abcdef\"") != std::string::npos);
}

TEST_CASE("Schema 1 files load through the migration", "[model][io][migration]") {
    const auto loaded = io::fromJson(kSchemaV1);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    requireValid(*loaded);

    const MediaItem& media = *loaded->findMedia(MediaId{1});
    CHECK(media.locator.path == "D:/footage/A.mov");
    CHECK(media.locator.relativePath.empty());
    CHECK_FALSE(media.locator.fingerprint.isKnown());
    REQUIRE(media.locator.proxy);
    CHECK(media.locator.proxy->path == "D:/proxies/A.mov");

    const Sequence& seq = *loaded->findSequence(SequenceId{2});
    CHECK(seq.duration == seq.contentEnd()); // schema 1 had no explicit duration
    CHECK(seq.duration == f(20)); // the audio clip ends at frame 20
    CHECK_FALSE(seq.workArea);
    const Clip& video = seq.videoTracks[0]->clips[0];
    CHECK(video.properties().value(ParamId::PositionX) == 10.0);
    CHECK(video.properties().value(ParamId::PositionY) == -20.5);
    CHECK(video.properties().value(ParamId::Opacity) == 0.5);
    CHECK(video.properties().isDefault(ParamId::Scale));
    CHECK(video.colorIndex == 2);
    const Clip& audio = seq.audioTracks[0]->clips[0];
    CHECK(audio.properties().value(ParamId::VolumeDb) == -6.0);
    CHECK(audio.properties().value(ParamId::Pan) == 0.25);
    CHECK(seq.videoTracks[0]->transitions.empty());

    // Saved again, it is a current-schema file that loads to the same project.
    const auto text = io::toJson(*loaded);
    REQUIRE(text);
    CHECK(text->find(kCurrentSchema) != std::string::npos);
    const auto reloaded = io::fromJson(*text);
    REQUIRE(reloaded);
    CHECK(*reloaded == *loaded);
}

TEST_CASE("Audio stream titles are saved when set and load back (A.10)", "[model][io]") {
    ProjectState s = richProject();
    {
        MediaItem item = *s.media.front();
        REQUIRE(item.info.audio.size() == 2);
        item.info.audio[0].title = "Game";
        item.info.audio[1].title = "Voice Chat";
        s.media.front() = std::make_shared<const MediaItem>(std::move(item));
    }
    const auto json = io::toJson(s);
    REQUIRE(json);
    CHECK(json->find("\"title\": \"Game\"") != std::string::npos);
    CHECK(json->find("\"title\": \"Voice Chat\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == s);
    CHECK(loaded->media.front()->info.audio[1].title == "Voice Chat");

    // Untitled streams write no title, and a file without titles (also one of schema 15, before A.10) loads its
    // streams as untitled.
    const auto plain = io::toJson(richProject());
    REQUIRE(plain);
    CHECK(plain->find("\"title\"") == std::string::npos);
    const auto older = io::fromJson(replaceOnce(*plain, kCurrentSchema, "\"schemaVersion\": 15"));
    INFO((older ? std::string() : older.error().message));
    REQUIRE(older);
    for (const MediaPtr& m : older->media) {
        for (const AudioStreamInfo& a : m->info.audio) {
            CHECK(a.title.empty());
        }
    }
}

TEST_CASE("Files from a newer schema are rejected with a clear error", "[model][io]") {
    const auto json = io::toJson(richProject());
    REQUIRE(json);
    const auto newer = io::fromJson(
        replaceOnce(*json, kCurrentSchema, "\"schemaVersion\": " + std::to_string(io::kSchemaVersion + 1)));
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == ErrorCode::VersionMismatch);
}

TEST_CASE("Damaged or foreign files are rejected", "[model][io]") {
    const auto json = io::toJson(richProject());
    REQUIRE(json);
    CHECK(loadError("{ not json") == ErrorCode::ParseError);
    CHECK(loadError("[]") == ErrorCode::ParseError);
    CHECK(loadError(R"({"format": "something-else", "schemaVersion": 2})") == ErrorCode::ParseError);
    CHECK(loadError(replaceOnce(*json, kCurrentSchema, "\"schemaVersion\": 0")) == ErrorCode::ParseError);
    CHECK(loadError(replaceOnce(*json, "\"nextId\"", "\"nextIdx\"")) == ErrorCode::ParseError);
    // Structurally invalid content (every id beyond the generator) is caught by validation.
    CHECK(loadError(replaceOnce(*json, "\"nextId\": ", "\"nextId\": 1, \"ignored\": ")) == ErrorCode::ParseError);
}

TEST_CASE("Out-of-domain values are rejected, not guessed", "[model][io]") {
    const auto json = io::toJson(richProject());
    REQUIRE(json);
    const std::string& j = *json;
    const std::string start = "\"start\": " + std::to_string(kTicksPerSecond * 3600 * 5 + 7);
    const std::vector<std::pair<std::string, std::string>> cases{
        {start, "\"start\": 1.5"},                                       // floating-point time
        {"\"transfer\": \"pq\"", "\"transfer\": \"bogus\""},             // unknown enum name
        {"\"alpha\": \"premultiplied\"", "\"alpha\": \"inverted\""},     // unknown enum name
        {"\"size\": 123456789012", "\"size\": -5"},                      // impossible file size
        {"\"sampleHash\": \"0123456789abcdef\"", "\"sampleHash\": \"XYZ\""}, // not a hash
        {"\"opacity.opacity\"", "\"opacity.transparency\""},            // unknown parameter
        {"\"rotationDegrees\": 90", "\"rotationDegrees\": 45"},         // not a quarter turn
        {"\"audioRate\": {", "\"audioRate\": {\"num\": 0, \"den\": 1}, \"previousAudioRate\": {"}, // zero rate
        {"\"channels\": 6", "\"channels\": -2"},
        {"\"role\": \"music\"", "\"role\": \"podcast\""},                // unknown track role
    };
    for (const auto& [from, to] : cases) {
        INFO("replacing " << from << " with " << to);
        CHECK(loadError(replaceOnce(j, from, to)) == ErrorCode::ParseError);
    }
    // Model invariants are enforced too: an opacity of 2 is outside the parameter's range.
    CHECK(loadError(replaceOnce(j, "\"value\": 0.3", "\"value\": 2.0")) == ErrorCode::ParseError);
}

TEST_CASE("Unknown keys are ignored", "[model][io]") {
    const ProjectState original = richProject();
    const auto json = io::toJson(original);
    REQUIRE(json);
    const auto loaded = io::fromJson(replaceOnce(*json, "\"bins\": [", "\"futureFeature\": {\"x\": [1, 2]}, \"bins\": ["));
    REQUIRE(loaded);
    CHECK(*loaded == original);
}

TEST_CASE("Saving keeps five rotated recovery copies of earlier saves", "[model][io][file]") {
    const fs::path dir = testDir("backups");
    const fs::path file = dir / u8"プロジェクト.wabcut";
    Timeline tl(R"(
        V1 |Aaaaaaaaaa|
    )");
    std::vector<ProjectState> saves;
    ProjectState s = tl.state;
    for (int i = 1; i <= 7; ++i) {
        s = ok(edit::markIn(s, tl.seq, f(i))).state;
        saves.push_back(s);
        REQUIRE(io::saveProject(s, file));
    }
    CHECK(*io::loadProject(file) == saves[6]);
    for (int n = 1; n <= 5; ++n) {
        const auto copy = io::loadProject(io::backupPath(file, n));
        INFO("backup " << n);
        REQUIRE(copy);
        CHECK(*copy == saves[static_cast<std::size_t>(6 - n)]); // .bak1 is the save before the last one
    }
    CHECK_FALSE(fs::exists(io::backupPath(file, 6)));
    CHECK_FALSE(fs::exists(fs::path(file).concat(".tmp")));

    // No copies when asked for none.
    const fs::path plain = dir / "plain.wabcut";
    REQUIRE(io::saveProject(saves[0], plain, io::SaveOptions{0}));
    REQUIRE(io::saveProject(saves[1], plain, io::SaveOptions{0}));
    CHECK_FALSE(fs::exists(io::backupPath(plain, 1)));
}

TEST_CASE("A damaged project file is not rotated into the recovery copies", "[model][io][file]") {
    const fs::path dir = testDir("damaged-rotation");
    const fs::path file = dir / "edit.wabcut";
    Timeline tl(R"(
        V1 |Aa|
    )");
    REQUIRE(io::saveProject(tl.state, file));
    REQUIRE(io::saveProject(tl.state, file)); // .bak1 = a valid save
    const std::string goodCopy = readFile(io::backupPath(file, 1));
    writeFile(file, "{ half a file");
    REQUIRE(io::saveProject(tl.state, file));
    CHECK(readFile(io::backupPath(file, 1)) == goodCopy); // the broken file did not push it out
    CHECK_FALSE(fs::exists(io::backupPath(file, 2)));
    CHECK(io::loadProject(file));
}

TEST_CASE("A project file another program changed is checked again before rotating, even at the same size",
          "[model][io][file]") {
    // P.1: saveProject skips re-parsing a file exactly as this process wrote or read it; any change means a full check.
    const fs::path dir = testDir("changed-rotation");
    const fs::path file = dir / "edit.wabcut";
    Timeline tl(R"(
        V1 |AaBb|
    )");
    const ProjectState a = ok(edit::markIn(tl.state, tl.seq, f(1))).state;
    const ProjectState b = ok(edit::markIn(tl.state, tl.seq, f(2))).state;
    REQUIRE(io::saveProject(a, file));
    REQUIRE(io::saveProject(a, file)); // .bak1 = a
    const std::string goodCopy = readFile(io::backupPath(file, 1));

    // Damaged in place, size unchanged: not rotated.
    std::string damaged = readFile(file);
    damaged[0] = 'X';
    writeFile(file, damaged);
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(2));
    REQUIRE(io::saveProject(a, file));
    CHECK(readFile(io::backupPath(file, 1)) == goodCopy);
    CHECK_FALSE(fs::exists(io::backupPath(file, 2)));

    // Replaced by another program with a valid project: rotated like any save.
    const std::string external = *io::toJson(b);
    writeFile(file, external);
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(4));
    REQUIRE(io::saveProject(a, file));
    CHECK(readFile(io::backupPath(file, 1)) == external);
    CHECK(readFile(io::backupPath(file, 2)) == goodCopy);
}

TEST_CASE("Saving refuses a state that would not load, touching nothing", "[model][io][file]") {
    const fs::path dir = testDir("refuse");
    const fs::path file = dir / "edit.wabcut";
    Timeline tl(R"(
        V1 |AaBb|
    )");
    REQUIRE(io::saveProject(tl.state, file));
    const auto before = snapshotDirectory(dir);

    auto seq = std::make_shared<Sequence>(tl.sequence());
    auto track = std::make_shared<Track>(*seq->videoTracks[0]);
    track->clips.mutableAt(1).timelineRange.start = f(1); // overlaps the first clip
    seq->videoTracks[0] = track;
    const auto r = io::saveProject(tl.state.withSequence(seq), file);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::InvalidArgument);
    CHECK(snapshotDirectory(dir) == before);
}

TEST_CASE("A failed load never touches any file", "[model][io][file]") {
    const fs::path dir = testDir("read-only");
    const fs::path file = dir / "edit.wabcut";
    Timeline tl(R"(
        V1 |Aa|
    )");
    REQUIRE(io::saveProject(tl.state, file));
    REQUIRE(io::saveProject(tl.state, file));
    REQUIRE(io::writeAutosave(tl.state, file));
    writeFile(file, replaceOnce(readFile(file), kCurrentSchema, "\"schemaVersion\": " + std::to_string(io::kSchemaVersion + 1)));
    const auto before = snapshotDirectory(dir);
    const auto r = io::loadProject(file);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::VersionMismatch);
    CHECK_FALSE(io::loadProject(dir / "missing.wabcut"));
    CHECK(snapshotDirectory(dir) == before);
}

TEST_CASE("Recovery candidates list the autosave and backups, newest first", "[model][io][file]") {
    const fs::path dir = testDir("recovery");
    const fs::path file = dir / "edit.wabcut";
    Timeline tl(R"(
        V1 |Aaaa|
    )");
    REQUIRE(io::saveProject(tl.state, file));
    REQUIRE(io::saveProject(tl.state, file)); // creates .bak1
    const auto edited = ok(edit::markIn(tl.state, tl.seq, f(2))).state;
    REQUIRE(io::writeAutosave(edited, file));
    // Make the timeline of events unambiguous whatever the file system's time resolution.
    const auto projectTime = fs::last_write_time(file);
    fs::last_write_time(io::backupPath(file, 1), projectTime - std::chrono::seconds(10));
    fs::last_write_time(io::autosavePath(file), projectTime + std::chrono::seconds(10));
    writeFile(io::backupPath(file, 2), "garbage");
    fs::last_write_time(io::backupPath(file, 2), projectTime - std::chrono::seconds(20));

    const auto candidates = io::recoveryCandidates(file);
    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0].kind == io::RecoveryCandidate::Kind::Autosave);
    CHECK(candidates[0].newerThanProject);
    CHECK(candidates[0].loadable);
    CHECK(candidates[1].kind == io::RecoveryCandidate::Kind::Backup);
    CHECK(candidates[1].backupIndex == 1);
    CHECK_FALSE(candidates[1].newerThanProject);
    CHECK(candidates[1].loadable);
    CHECK(candidates[2].backupIndex == 2);
    CHECK_FALSE(candidates[2].loadable);
    CHECK(*io::loadProject(candidates[0].file) == edited);
}

TEST_CASE("Autosave writes next to the project, only when something changed", "[model][io][file]") {
    const fs::path dir = testDir("autosave");
    const fs::path file = dir / "edit.wabcut";
    CHECK(io::autosavePath(file) == dir / "edit.wabcut.autosave");

    Timeline tl(R"(
        V1 |Aaaa|
    )");
    auto snapshot = std::make_shared<const ProjectState>(tl.state);
    io::Autosaver autosaver(file);
    const auto firstWrite = autosaver.save(snapshot);
    REQUIRE(firstWrite);
    CHECK(*firstWrite);
    CHECK(fs::exists(io::autosavePath(file)));
    CHECK_FALSE(fs::exists(file)); // the project file itself is not touched
    CHECK_FALSE(fs::exists(io::backupPath(io::autosavePath(file), 1))); // autosaves are not rotated

    const auto unchanged = autosaver.save(std::make_shared<const ProjectState>(*snapshot)); // same revision
    REQUIRE(unchanged);
    CHECK_FALSE(*unchanged);

    auto edited = std::make_shared<const ProjectState>(ok(edit::markIn(tl.state, tl.seq, f(1))).state);
    const auto secondWrite = autosaver.save(edited);
    REQUIRE(secondWrite);
    CHECK(*secondWrite);
    const auto restored = io::loadProject(io::autosavePath(file));
    REQUIRE(restored);
    CHECK(*restored == *edited);

    autosaver.markSaved(std::make_shared<const ProjectState>(*edited));
    const auto afterManualSave = autosaver.save(edited);
    REQUIRE(afterManualSave);
    CHECK_FALSE(*afterManualSave);
}

TEST_CASE("Schema 7 files get transition alignments derived from their sides", "[model][io][migration]") {
    Timeline tl(R"(
        V1 tgt |A100aaaaaaB100bbbbbbC100ccccccD100ddddddE100eeeeee|
    )");
    ProjectState s = tl.state;
    {
        // Written without the operations, so only the migration decides the alignment.
        auto seq = std::make_shared<Sequence>(*s.findSequence(tl.seq));
        auto v1 = std::make_shared<Track>(*seq->videoTracks[0]);
        const std::pair<std::int64_t, std::int64_t> sides[] = {{0, 3}, {3, 0}, {2, 3}, {1, 4}};
        for (std::size_t i = 0; i < 4; ++i) {
            v1->transitions.push_back(Transition{s.ids.allocate<TransitionId>(), v1->clips[i].id, v1->clips[i + 1].id,
                                                 f(sides[i].first), f(sides[i].second)});
        }
        seq->videoTracks[0] = v1;
        const IdGenerator ids = s.ids;
        s = s.withSequence(seq);
        s.ids = ids;
    }
    const auto json = io::toJson(s);
    REQUIRE(json);
    const std::string v7 = std::regex_replace(replaceOnce(*json, kCurrentSchema, "\"schemaVersion\": 7"),
                                              std::regex(R"("alignment": "[A-Za-z]+",\s*)"), "");
    REQUIRE(v7.find("\"alignment\": \"") == std::string::npos);
    const auto loaded = io::fromJson(v7);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    std::vector<TransitionAlignment> alignments;
    for (const Transition& t : loaded->findSequence(tl.seq)->videoTracks[0]->transitions) {
        alignments.push_back(t.alignment);
    }
    CHECK(alignments == std::vector<TransitionAlignment>{TransitionAlignment::StartAtCut, TransitionAlignment::EndAtCut,
                                                         TransitionAlignment::Center, TransitionAlignment::Custom});
    // A schema 8 file must say it.
    const std::string missing = std::regex_replace(*json, std::regex(R"("alignment": "[A-Za-z]+",\s*)"), "");
    CHECK(loadError(missing) == ErrorCode::ParseError);
}

TEST_CASE("Project files nested deeper than any project are refused, not a stack overflow (C.10)", "[model][io]") {
    // A clip source of an unknown kind is kept as JSON text: 100,000 levels of it would overflow the stack writing it.
    Timeline plain(R"(
        V1 tgt |Aaaa|
    )");
    std::string text = *io::toJson(plain.state);
    const std::string media = "\"media\": " + std::to_string(plain.media('A').value);
    const auto at = text.find(media);
    REQUIRE(at != std::string::npos);
    const std::string deep = std::string(100'000, '[') + std::string(100'000, ']');
    std::string hostile = text;
    hostile.replace(at, media.size(), R"("source": {"type": "hologram", "layers": )" + deep + "}");
    const auto refused = io::fromJson(hostile);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::ParseError);
    CHECK_FALSE(io::fromJson(deep));
    // Brackets inside strings do not count, and real nesting up to the bound loads.
    CHECK(io::jsonNestingWithin(R"({"name": "[[[[[[[[[[[[ \" {{{{{{{{"})", 2));
    CHECK_FALSE(io::jsonNestingWithin("[[[", 2));
    const std::size_t depth = io::kMaxJsonNesting - 32; // the clip itself is about 10 deep
    const std::string allowed = std::string(depth, '[') + std::string(depth, ']');
    std::string nested = text;
    nested.replace(at, media.size(), R"("source": {"type": "hologram", "layers": )" + allowed + "}");
    const auto loaded = io::fromJson(nested);
    REQUIRE(loaded);
    CHECK(*io::fromJson(*io::toJson(*loaded)) == *loaded);
}

TEST_CASE("A project file cannot set the id generator where it would wrap around (C.10)", "[model][io]") {
    Timeline plain(R"(
        V1 tgt |Aaaa|
    )");
    std::string text = *io::toJson(plain.state);
    const std::string next = "\"nextId\": " + std::to_string(plain.state.ids.next);
    const auto at = text.find(next);
    REQUIRE(at != std::string::npos);
    text.replace(at, next.size(), "\"nextId\": 18446744073709551615");
    const auto refused = io::fromJson(text);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("out of range") != std::string::npos);
}

TEST_CASE("A project file cannot hold clip times that would wrap around int64 (C.11)", "[model][io]") {
    // The clipboard fuzzer: a clip of duration 2^63 - 1 loaded, and every edit adding to its end then wrapped.
    Timeline plain(R"(
        V1 tgt |AaaaBbbb|
    )");
    std::string text = *io::toJson(plain.state);
    const std::string duration = "\"duration\": " + std::to_string(f(4).value);
    const auto at = text.rfind(duration);
    REQUIRE(at != std::string::npos);
    text.replace(at, duration.size(), "\"duration\": 9223372036854775807");
    const auto refused = io::fromJson(text);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("beyond the longest timeline") != std::string::npos);
}

namespace {

/// Every object key of JSON `text`: in valid JSON a string followed by ':' is always a key.
std::set<std::string> collectKeys(const std::string& text) {
    static const std::regex kKey(R"rx("((?:[^"\\]|\\.)*)"\s*:)rx");
    std::set<std::string> keys;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), kKey); it != std::sregex_iterator(); ++it) {
        keys.insert((*it)[1].str());
    }
    return keys;
}

} // namespace

TEST_CASE("Every key a project file uses is in the key list of its schema (E5-P2-8)", "[model][io][migration]") {
    // The rich project with the fields of A.8, A.9, C.13 and I.9 too. A feature that adds a field adds it here, bumps
    // kSchemaVersion (with a migration step and a note in ProjectIO.h) and updates tests/data/project-file-keys.txt:
    // files with the new field must not half-load in the builds before it.
    ProjectState s = richProject();
    const SequenceId seq = s.sequences.front()->id;
    const ClipId audio = s.findSequence(seq)->audioTracks.at(0)->clips.front().id;
    const TrackId v1 = s.findSequence(seq)->videoTracks.at(0)->id;
    s = ok(edit::setClipChannels(s, seq, std::span(&audio, 1), AudioChannelMap::swap())).state;
    s = ok(edit::setMasterFormat(s, seq, ChannelFormat::Surround51)).state;
    s = ok(edit::setSequenceStartTimecode(s, seq, Ticks::fromSeconds(3600))).state; // X.1, schema 14
    {
        edit::SequenceSettingsChange label; // 0.2, schema 18: a sequence's label colour
        label.colorIndex = 4;
        s = ok(edit::setSequenceSettings(s, seq, label)).state;
    }
    // A.8 and A.9's fields that are written only when set: a record-armed track with a moved 5.1 panner, a take.
    const TrackId a1 = s.findSequence(seq)->audioTracks.at(0)->id;
    edit::TrackStateChange armed;
    armed.recordArmed = true;
    armed.surround = SurroundPan{ParamValue(-0.5), ParamValue(0.5), ParamValue(-12.0)};
    s = ok(edit::setTrackState(s, seq, a1, armed)).state;
    {
        MediaItem take = *s.media.front();
        take.recording = RecordingProvenance{seq, a1, TimeRange{f(0), f(2)}, 1, "USB Mic", f(0), "2026-10-12T09:00:00Z"};
        take.info.audio.front().title = "Mic"; // A.10, schema 16: a named stream
        if (!take.info.video.empty()) {
            take.info.video.front().logColorSpace = "s-log3-cine"; // 0.2, schema 17: the camera log its metadata names
            take.info.video.front().fieldOrder = FieldOrder::UpperFirst; // 0.2 media, schema 18: interlaced footage
        }
        s.media.front() = std::make_shared<const MediaItem>(std::move(take));
    }
    requireValid(s);
    Graphic g;
    GraphicLayer title;
    title.id = 1;
    title.kind = LayerKind::Text;
    title.text.runs = {TextRun{"Title", TextStyle{}}};
    g.layers = {title};
    s = ok(edit::addGraphic(s, seq, v1, f(8), f(2), g)).state;
    BrandKit kit;
    kit.colors = {BrandColor{"Primary", {1.0, 0.0, 0.0, 1.0}}};
    s = ok(edit::setBrandKit(s, kit)).state;
    auto tmpl = edit::makeTemplate(g, "title", "Title", {TemplateField{"text", "Text", FieldKind::Text, 1, "text", {}}});
    REQUIRE(tmpl);
    s = ok(edit::saveProjectTemplate(s, *tmpl)).state;
    // C.15 (schema 15): a clip blend mode, a synthetic item with parameters and an adjustment layer.
    const ClipId video = s.findSequence(seq)->videoTracks.at(0)->clips.front().id;
    edit::ClipAttributeChange blend;
    blend.blendMode = BlendMode::Overlay;
    s = ok(edit::setClipAttributes(s, seq, std::span(&video, 1), blend)).state;
    auto matte = edit::addSyntheticMedia(s, SyntheticKind::ColorMatte, {EffectParam{"color.r", 0.5}}, seq);
    REQUIRE(matte);
    s = matte->state;
    s = ok(edit::addAdjustmentLayer(s, seq, v1, f(12), f(2))).state;
    // P.2 (schema 17): a clip's Time Interpolation.
    s = ok(edit::setTimeInterpolation(s, seq, std::span(&video, 1), TimeInterpolation::OpticalFlow)).state;
    Marker highlighted{f(1), f(2), "Clutch"};
    highlighted.highlight = MarkerHighlight{0.9, HighlightConfidence::High, {"kill streak"}, f(2), "highlights"};
    s = ok(edit::addMarker(s, seq, highlighted)).state;
    // P.2 (schema 17): Scale to Frame Size and a frame hold with Hold Filters, an item's default scaling, interpretation,
    // timecode, still duration and Make Offline, the sequence's pixel aspect, fields, preview format and display
    // formats, and a merged clip.
    s = ok(edit::setScaleToFrameSize(s, seq, std::span(&video, 1), true)).state;
    ClipFieldOptions fields; // 0.2 media (schema 18): Field Options
    fields.reverseFieldDominance = true;
    fields.processing = FieldProcessing::AlwaysDeinterlace;
    s = ok(edit::setFieldOptions(s, seq, std::span(&video, 1), fields)).state;
    edit::FrameHoldOptions hold;
    hold.holdFilters = true;
    s = ok(edit::setFrameHoldOptions(s, seq, video, hold)).state;
    const auto named = [&](std::string_view name) {
        const auto it = std::find_if(s.media.begin(), s.media.end(), [&](const MediaPtr& m) { return m->name == name; });
        REQUIRE(it != s.media.end());
        return it;
    };
    {
        const auto interview = named("Interview 🎤"); // an item with a file and no clips
        MediaItem item = **interview;
        item.scaling = MediaScaling::SetToFill;
        MediaInterpretation in;
        in.frameRate = Rate{24, 1};
        in.pixelAspect = Rate{1, 1};
        in.fieldOrder = FieldOrder::UpperFirst;
        in.ignoreAlpha = true;
        in.invertAlpha = true;
        in.premultiplication = AlphaMode::Straight;
        item.interpretation = in;
        item.timecode = MediaTimecode{Ticks::fromSeconds(3600), "A001"};
        item.stillDuration = Ticks::fromSeconds(3);
        item.madeOffline = true;
        *interview = std::make_shared<const MediaItem>(std::move(item));
    }
    edit::SequenceSettingsChange settings;
    settings.pixelAspect = Rate{2, 1};
    settings.fields = FieldOrder::LowerFirst;
    settings.previewFormat = "prores";
    settings.videoDisplay = VideoTimeDisplay::Feet35mm;
    settings.audioDisplay = AudioTimeDisplay::Milliseconds;
    s = ok(edit::setSequenceSettings(s, seq, settings)).state;
    edit::MergeSpec merge;
    merge.video = (*named("A"))->id;
    merge.audio = {(*named("M"))->id};
    auto merged = edit::mergeClips(s, merge);
    REQUIRE(merged);
    s = merged->state;
    // p02 audio (schema 17): a submix track, a track assigned to it and a pre-fader, muted send with a level and pan.
    s = ok(edit::addSubmixTrack(s, seq, ChannelFormat::Stereo, "Voices")).state;
    const TrackId submix = s.findSequence(seq)->submixTracks.at(0)->id;
    s = ok(edit::setTrackOutput(s, seq, a1, submix)).state;
    TrackSend send;
    send.submix = submix;
    send.preFader = true;
    send.muted = true;
    send.level = ParamValue(-6.0);
    send.pan = ParamValue(0.5);
    s = ok(edit::addTrackSend(s, seq, a1, send)).state;
    // 0.2 (schema 17): a rolling graphic with a layer pinned to the video frame and a text style it follows, and a caption
    // track following a caption preset (its Track Style).
    {
        Graphic credits;
        GraphicLayer name;
        name.id = 1;
        name.kind = LayerKind::Text;
        name.text.runs = {TextRun{"Directed by", TextStyle{}}};
        name.pin = LayerPin{LayerPin::kVideoFrame, LayerPin::kLeft, {40.0, 0.0, 0.0, 0.0}};
        // 0.2 graphics (schema 18): a text layer on a path with every option off its default.
        GraphicLayer ring;
        ring.id = 2;
        ring.kind = LayerKind::Text;
        ring.text.runs = {TextRun{"Around", TextStyle{}}};
        TextPath path;
        path.path = {MaskPathKey{Ticks{}, {MaskVertex{-100, 0, 0, -50, 0, 0}, MaskVertex{100, 0, 0, -50, 0, 0}},
                                 Interpolation::Linear}};
        path.reverse = true;
        path.perpendicular = false;
        path.forceAlignment = true;
        path.firstMargin = 10.0;
        path.lastMargin = 20.0;
        ring.text.path = path;
        credits.layers = {name, ring};
        credits.roll = GraphicRoll{};
        auto rolled = ok(edit::addGraphic(s, seq, v1, f(11), f(2), credits));
        s = ok(edit::createTextStyle(rolled.state, seq, rolled.placed.front(), 1, "Credits")).state;
        auto captions = edit::addCaptionTrack(s, seq, "English", "en");
        REQUIRE(captions);
        s = *edit::saveCaptionPreset(captions->state, CaptionPreset{"Channel look", CaptionStyle{}});
        s = ok(edit::setCaptionTrackStyle(s, seq, s.findSequence(seq)->captionTracks.back().id, "Channel look")).state;
    }
    // 0.2 P.3 (schema 18): clip metadata, a search bin with every field and a Freeform view with a saved layout.
    {
        edit::MediaAttributeChange md;
        md.scene = "12";
        md.shot = "3";
        md.take = "2";
        md.logNote = "Good energy";
        md.client = "Northwind";
        md.good = true;
        md.status = "Approved";
        md.cameraAngle = "Wide";
        md.cameraLabel = "A";
        md.cameraModel = "FX6";
        md.shotDate = "2026-10-16";
        md.shotLocation = "Kitchen";
        s = *edit::setMediaAttributes(s, std::vector<MediaId>{s.media.front()->id}, md);
        // A copy from another project, on a media item and a sequence.
        MediaItem copied = *s.media.front();
        copied.importSource = ImportSource{"D:/Other.wabcut", 12};
        s.media.front() = std::make_shared<const MediaItem>(std::move(copied));
        Sequence importedSeq = *s.sequences.front();
        importedSeq.importSource = ImportSource{"D:/Other.wabcut", 13};
        s = s.withSequence(std::make_shared<const Sequence>(std::move(importedSeq)));
        BinSearch query{"interview", "scene", "video", 2, true, true, true, true, true, true, true, "2026-01-01", "2026-12-31"};
        s = edit::addSearchBin(s, "Selects", query)->state;
        FreeformArrangement arrangement;
        arrangement.scale = 1.25;
        FreeformPlacement p;
        p.kind = FreeformPlacement::Kind::Media;
        p.id = s.media.front()->id.value;
        p.x = 10.0;
        p.y = 20.0;
        p.size = 2.0;
        arrangement.items = {p};
        s = *edit::setFreeformArrangement(s, s.media.front()->bin, arrangement);
        s = *edit::saveFreeformLayout(s, s.media.front()->bin, "Storyboard");
    }
    requireValid(s);
    // I.9: an HDR project and a clip read as camera log.
    ColorManagement hdr;
    hdr.mode = ColorManagementMode::HdrPq;
    hdr.peakNits = 4000.0;
    hdr.autoDetectLog = false; // 0.2 (schema 17): written only when off
    auto managed = edit::setColorManagement(s, hdr);
    REQUIRE(managed);
    s = *managed;
    s = ok(edit::setClipInputColorSpace(s, seq, std::span(&video, 1), InputColorSpace::SLog3SGamut3Cine)).state;
    // p03 AI (schema 18): an object mask made from a nested clip's picture names its sequence instead of media.
    {
        Mask object;
        object.name = "Object";
        object.shape = MaskShape::Matte;
        object.matte = MaskMatte{"0000000000abcdef/0/red-v1/0123456789abcdef", MediaId{}, 0, TimeRange{f(0), f(10)}, "red-v1", seq};
        for (const char* key : {"center.x", "center.y", "size.x", "size.y"}) {
            object.set(key, ParamValue(0.5));
        }
        auto masked = edit::addMask(s, seq, video, {}, object);
        REQUIRE(masked);
        s = masked->state;
    }

    const auto text = io::toJson(s);
    REQUIRE(text);
    const std::set<std::string> keys = collectKeys(*text);
    std::string current = "schema " + std::to_string(io::kSchemaVersion) + "\n";
    for (const std::string& k : keys) {
        current += k + "\n";
    }
    const fs::path golden = fs::path(WABCUT_TEST_DATA_DIR) / "project-file-keys.txt";
    std::string expected = readFile(golden);
    std::erase(expected, '\r');
    if (current != expected) {
        // The list as it is now, to compare and copy over once the schema is bumped.
        writeFile(fs::path(WABCUT_TEST_OUTPUT_DIR) / "project-file-keys.txt", current);
    }
    INFO("project file keys changed: bump kSchemaVersion and update " << pathToUtf8(golden) << " (the new list is in "
                                                                       << WABCUT_TEST_OUTPUT_DIR << ")");
    CHECK(current == expected);
}
