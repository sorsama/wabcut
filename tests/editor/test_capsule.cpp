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
#include <wabcut/model/Auditions.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RenderReplace.h>
#include <wabcut/model/SpectralRepair.h>
#include <wabcut/model/TimeRemap.h>

#include <nlohmann/json.hpp>

#include <QGuiApplication>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <streambuf>
#include <thread>

using namespace wabcut;
using Catch::Approx;
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
        CHECK((versions[i - 1].summary.modified >= versions[i].summary.modified)); // newest first after Current (a bool: libc++'s file times do not print)
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

// 0.7 (card 047): Spectral Repair's edits are seconds of the source, in the effect's data. The trimmed copy starts later in
// the original, so every edit moves earlier by the same shift; one that ends before the copy begins goes.
TEST_CASE("Capsule: trimmed copies move Spectral Repair edits with the media (card 047)", "[editor][capsule][spectral]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-spectral");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    SpectralEdit click;
    click.kind = SpectralKind::Heal;
    click.start = 2.40;
    click.end = 2.45;
    click.low = 800.0;
    click.high = 4000.0;
    SpectralEdit early = click; // before the used range (2 s to 3 s) and its handles
    early.start = 0.20;
    early.end = 0.25;
    SpectralEdit hum;
    hum.kind = SpectralKind::Tone;
    hum.start = 2.1;
    hum.end = 2.9;
    hum.path = {SpectralPoint{2.1, 120.0}, SpectralPoint{2.9, 130.0}};
    // The effect is an audio effect: it goes on the audio clip of the same file.
    ClipId audioClip;
    for (const TrackPtr& track : session.snapshot()->findSequence(p.sequence)->audioTracks) {
        for (const Clip& c : track->clips) {
            if (c.media == p.h264) {
                audioClip = c.id;
            }
        }
    }
    REQUIRE(audioClip.isValid());
    auto edited = session.apply("Spectral Repair", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        auto added = edit::addEffect(state, p.sequence, edit::EffectTarget::ofClip(audioClip), std::string(kSpectralRepairEffect), {}, 0);
        if (!added) {
            return std::unexpected(added.error());
        }
        edit::EffectChange change;
        change.data = packSpectralEdits(std::vector<SpectralEdit>{early, click, hum});
        auto set = edit::setEffect(added->state, p.sequence, audioClip, added->createdEffects.front(), change);
        if (!set) {
            return std::unexpected(set.error());
        }
        return set->state;
    });
    REQUIRE(edited);
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Spectral";
    options.trim = true;
    options.handles = Ticks{kTicksPerSecond / 2};
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    REQUIRE(r->trimmedFiles >= 1);

    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    const ProjectState& state = *reopened.snapshot();
    const auto audioOf = [&](const ProjectState& st) -> const Clip& {
        const auto found = st.findSequence(p.sequence)->findClip(audioClip);
        REQUIRE(found);
        return found->clip();
    };
    const Clip& before = audioOf(original);
    const Clip& after = audioOf(state);
    const double shift = (before.sourceIn - after.sourceIn).toSeconds();
    REQUIRE(shift > 0.5); // the file was cut at its start (the used range begins at 2 s, less the handles and the codec's frame boundary)
    REQUIRE(shift < click.start - 0.1); // and not into the repair
    REQUIRE(after.effects().size() == 1);
    const std::vector<SpectralEdit> moved = unpackSpectralEdits(after.effects().front());
    REQUIRE(moved.size() == 2); // the early edit ended before the copy begins
    CHECK(moved[0].kind == SpectralKind::Heal);
    CHECK(moved[0].start == Approx(click.start - shift).margin(1e-3));
    CHECK(moved[0].end == Approx(click.end - shift).margin(1e-3));
    CHECK(moved[0].low == Approx(800.0));
    CHECK(moved[1].kind == SpectralKind::Tone);
    CHECK(moved[1].start == Approx(hum.start - shift).margin(1e-3));
    REQUIRE(moved[1].path.size() == 2);
    CHECK(moved[1].path[0].time == Approx(hum.path[0].time - shift).margin(1e-3));
    CHECK(moved[1].path[1].hz == Approx(130.0));
}

// 0.7 (card 069): a rendered-and-replaced clip keeps the way back to its nest (RenderedNest::origin, where the rendered file
// starts in the nest's time). Collect Files cuts the file's start off, so the origin moves with it and Restore Unrendered in
// the collected copy puts the nest back exactly where it was.
TEST_CASE("Capsule: a rendered clip's way back follows the trimmed file (card 069)", "[editor][capsule][render-replace]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-rendered");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    // Nest the H.264 clip, shorten the nest's clip at its head (it plays the nest from 0.4 s), then swap it for a file as a
    // render would (here the H.264 file itself: only the bookkeeping of time matters), with no handles: the file starts where
    // the nest does.
    ClipId nestedClip;
    SequenceId nest;
    REQUIRE(session.apply("Nest", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        const ClipId ids[] = {p.h264Clip};
        auto r = edit::nestClips(state, p.sequence, ids, "Nest");
        if (!r) {
            return std::unexpected(r.error());
        }
        nestedClip = r->clips.front();
        nest = r->nested;
        return r->edit.state;
    }));
    REQUIRE(session.apply("Trim", [&](const ProjectState& state) {
        return edit::trimEdge(state, p.sequence, nestedClip, edit::Edge::Head, Ticks{kTicksPerSecond * 4 / 10});
    }));
    const Clip nestedBefore = clipOf(*session.snapshot(), p.sequence, nestedClip);
    REQUIRE(nestedBefore.isNested());
    MediaItem file = *session.snapshot()->findMedia(p.h264);
    file.name = "Rendered";
    REQUIRE(session.apply("Render and Replace", [&](const ProjectState& state) {
        return edit::renderAndReplace(state, p.sequence, std::vector<ClipId>{nestedClip}, file, Ticks{});
    }));
    // The clip plays the file from 2.2 s (the file is long enough, as a render of a long nest would be).
    REQUIRE(session.apply("Slip", [&](const ProjectState& state) {
        return edit::slip(state, p.sequence, std::vector<ClipId>{nestedClip}, Ticks{kTicksPerSecond * 18 / 10});
    }));
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();
    const auto restoredBefore = edit::restoreUnrendered(original, p.sequence, std::vector<ClipId>{nestedClip});
    REQUIRE(restoredBefore);
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Rendered";
    options.trim = true;
    options.handles = Ticks{kTicksPerSecond / 10};
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);

    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    const Clip& before = clipOf(original, p.sequence, nestedClip);
    const Clip& after = clipOf(*reopened.snapshot(), p.sequence, nestedClip);
    REQUIRE(before.renderedNest() != nullptr);
    REQUIRE(after.renderedNest() != nullptr);
    const Ticks shift = before.sourceIn - after.sourceIn;
    CHECK(shift > Ticks{}); // the file was cut at its start
    CHECK(after.renderedNest()->origin == before.renderedNest()->origin + shift);
    // Restore Unrendered in the copy: the nest plays from exactly where it does when the project is restored uncollected.
    const auto restored = edit::restoreUnrendered(*reopened.snapshot(), p.sequence, std::vector<ClipId>{nestedClip});
    REQUIRE(restored);
    const Clip& back = clipOf(restored->state, p.sequence, nestedClip);
    const Clip& expected = clipOf(restoredBefore->state, p.sequence, nestedClip);
    CHECK(back.isNested());
    CHECK(back.sourceIn == expected.sourceIn);
    CHECK(back.sourceIn == nestedBefore.sourceIn + Ticks{kTicksPerSecond * 18 / 10});
    CHECK(back.timelineRange == nestedBefore.timelineRange);
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

// Fix round 1 (ARGON-03): media only an audition's other take plays is used. Collect Files used to leave it out (with
// "exclude unused") and save a project whose take pointed at nothing; trimmed, the take's In follows its own copy.
TEST_CASE("Capsule: an audition's other take keeps its media and its In follows the trimmed copy",
          "[editor][capsule][auditions]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-audition");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    fs::copy_file(testMedia("prores_422.mov"), dir / "source" / "media" / "alt.mov", fs::copy_options::overwrite_existing);
    const MediaId alt = importOne(session, dir / "source" / "media" / "alt.mov", p.file);
    edit::TakeSource source;
    source.media = alt;
    source.sourceIn = Ticks{kTicksPerSecond / 2};
    source.name = "Alt";
    auto added = session.apply("Add take", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto r = edit::addAuditionSource(s, p.sequence, p.h264Clip, source);
        if (!r) {
            return std::unexpected(r.error());
        }
        return r->state;
    });
    REQUIRE(added);
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();
    const Audition* before = clipOf(original, p.sequence, p.h264Clip).audition();
    REQUIRE(before != nullptr);
    REQUIRE(before->others.size() == 1);
    REQUIRE(before->others.front().media == alt);

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Takes";
    options.trim = true;
    options.handles = Ticks{kTicksPerSecond / 4};
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    INFO("offline after reopen " << r->offlineAfterReopen << ", notes " << (r->notes.empty() ? std::string() : r->notes.front()));
    CHECK(r->verified);
    CHECK(r->mediaFiles == 3); // H.264, ProRes and the take's file; the WAV is not used
    CHECK(fs::is_regular_file(r->folder / "Media" / "alt.mov"));

    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    CHECK(copy.offlineMedia().empty());
    const ProjectState& state = *reopened.snapshot();
    REQUIRE(state.findMedia(alt) != nullptr);
    const Audition* after = clipOf(state, p.sequence, p.h264Clip).audition();
    REQUIRE(after != nullptr);
    REQUIRE(after->others.size() == 1);
    // The take shows source 0.5 s to 1.5 s; its copy keeps 0.25 s to 1.75 s, so its In moves 0.25 s earlier (to the
    // frame the copy starts on).
    if (r->trimmedFiles == 3) {
        const Ticks shift = before->others.front().sourceIn - after->others.front().sourceIn;
        CHECK(shift > Ticks{});
        CHECK(shift < Ticks{kTicksPerSecond / 2});
        CHECK(after->others.front().sourceIn >= Ticks{});
    }
}

// ---- 0.7, stream C, batch 2 ------------------------------------------------------------------------------------------

namespace trimc2 {

/// A stream that reads `data` and then ends as if the file did (a share that dropped: MSVC reports a read error as the
/// end of the file), or fails with an I/O error (`error`).
class DroppingBuf : public std::streambuf {
public:
    DroppingBuf(std::string data, bool error) : m_data(std::move(data)), m_error(error) {}

protected:
    int_type underflow() override {
        if (m_given < m_data.size()) {
            const std::size_t n = std::min<std::size_t>(m_data.size() - m_given, 1000);
            setg(m_data.data() + m_given, m_data.data() + m_given, m_data.data() + m_given + n);
            m_given += n;
            return traits_type::to_int_type(*gptr());
        }
        if (m_error) {
            throw std::ios_base::failure("read error");
        }
        return traits_type::eof();
    }

private:
    std::string m_data;
    std::size_t m_given = 0;
    bool m_error;
};

Ticks secs(double seconds) {
    return Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
}

/// A saved edit of the project state (the redactions, effects and markers the cards are about).
void editState(EditSession& session, const std::function<void(ProjectState&)>& fn) {
    auto r = session.apply("Edit project", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        ProjectState next = state;
        fn(next);
        return next;
    });
    REQUIRE(r);
}

Curve curveAt(std::initializer_list<std::pair<double, double>> secondsAndValues) {
    Curve c;
    for (const auto& [seconds, value] : secondsAndValues) {
        c.keys.push_back(Keyframe{secs(seconds), value});
    }
    return c;
}

Mask pathMask(std::uint32_t id, double firstSeconds, double secondSeconds) {
    Mask m;
    m.id = id;
    m.name = "Face";
    m.shape = MaskShape::Path;
    const std::vector<MaskVertex> triangle{{0.2, 0.2}, {0.8, 0.2}, {0.5, 0.8}};
    m.path.push_back(MaskPathKey{secs(firstSeconds), triangle});
    m.path.push_back(MaskPathKey{secs(secondSeconds), triangle});
    m.set("opacity", ParamValue(curveAt({{firstSeconds, 0.5}, {secondSeconds, 1.0}})));
    return m;
}

EffectInstance blurWith(EffectInstanceId id, double keySeconds, const Mask& mask) {
    EffectInstance e;
    e.id = id;
    e.effect = "wabcut.blur.gaussian";
    e.set("radius", ParamValue(curveAt({{keySeconds, 4.0}, {keySeconds + 0.2, 8.0}})));
    e.masks.push_back(mask);
    return e;
}

} // namespace trimc2

// Card #90: Collect Files copied a file to the end of what it could read and called it done.
TEST_CASE("Collect Files copy is the whole file or an error (090)", "[editor][capsule]") {
    using namespace trimc2;
    const std::string data(150'000, 'x');
    const auto copy = [&](std::istream& in, std::int64_t expected, std::ostream& out, std::stop_token stop = {}) {
        std::int64_t seen = 0;
        auto r = wabcut::editor::detail::copyChecked(in, out, expected, stop, [&](std::int64_t n) { seen += n; });
        return std::pair{std::move(r), seen};
    };
    {   // The whole file.
        std::istringstream in(data);
        std::ostringstream out;
        auto [r, seen] = copy(in, 150'000, out);
        REQUIRE(r);
        CHECK(*r == 150'000);
        CHECK(seen == 150'000);
        CHECK(out.str() == data);
    }
    {   // An empty file is a whole file.
        std::istringstream in{std::string()};
        std::ostringstream out;
        auto [r, seen] = copy(in, 0, out);
        REQUIRE(r);
        CHECK(*r == 0);
    }
    {   // The read ends early like an end of file (the drive dropped): fewer bytes than the file has.
        DroppingBuf buf(data.substr(0, 61'234), false);
        std::istream in(&buf);
        std::ostringstream out;
        auto [r, seen] = copy(in, 150'000, out);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::IoError);
        CHECK(r.error().message.find("61234") != std::string::npos);
    }
    {   // A read error in the middle (badbit) with the right byte count so far is still an error.
        DroppingBuf buf(data, true);
        std::istream in(&buf);
        std::ostringstream out;
        auto [r, seen] = copy(in, 150'000, out);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::IoError);
    }
    {   // The file grew while it was copied: not the file that was measured.
        std::istringstream in(data);
        std::ostringstream out;
        auto [r, seen] = copy(in, 100'000, out);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::IoError);
    }
    {   // The destination refuses the bytes.
        std::istringstream in(data);
        std::ostringstream out;
        out.setstate(std::ios::badbit);
        auto [r, seen] = copy(in, 150'000, out);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::IoError);
    }
    {   // Cancel.
        std::istringstream in(data);
        std::ostringstream out;
        std::stop_source source;
        source.request_stop();
        auto [r, seen] = copy(in, 150'000, out, source.get_token());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::Cancelled);
    }
}

TEST_CASE("Collect Files copies whole files byte for byte and keeps no half-copy (090)", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-copy-check");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    const ProjectState original = session.stateForSave();
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Whole";
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->verified);
    CHECK(fs::file_size(r->folder / "Media" / "h264_1080p60_aac.mp4") == fs::file_size(testMedia("h264_1080p60_aac.mp4")));
    for (const auto& entry : fs::directory_iterator(dir / "out")) {
        CHECK(entry.path().filename().string().find("wabcut-partial") == std::string::npos);
    }
}

// Card #89: a trimmed copy starts `shift` earlier than the original; everything keyed to the media's own time follows.
TEST_CASE("Collect Files trims redactions, masks, master effects, markers and transcript edits with the media (089)",
          "[editor][capsule]") {
    using namespace trimc2;
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-trim-media-time");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source"); // the H.264 clip is used from 2 s to 3 s
    editState(session, [&](ProjectState& s) {
        // A clip effect with a tracked mask (media time), on the used clip.
        for (SequencePtr& seq : s.sequences) {
            auto copy = std::make_shared<Sequence>(*seq);
            for (TrackPtr& track : copy->videoTracks) {
                std::vector<Clip> clips(track->clips.begin(), track->clips.end());
                for (Clip& c : clips) {
                    if (c.id == p.h264Clip) {
                        EffectStack effects = c.effects();
                        effects.push_back(blurWith(s.ids.allocate<EffectInstanceId>(), 2.2, pathMask(1, 2.2, 2.8)));
                        c.setEffects(std::move(effects));
                    }
                }
                auto next = std::make_shared<Track>(*track);
                next->clips = ClipList(std::move(clips));
                track = std::move(next);
            }
            seq = std::move(copy);
        }
        // A redaction (source time) with a keyed region and a bleep over part of it, a later one past the used part and
        // one wholly before it; Master Clip Effects; markers; marks; transcript edits.
        Redaction plate;
        plate.id = s.ids.allocate<RedactionId>();
        plate.media = p.h264;
        plate.name = "Licence plate";
        plate.source = TimeRange::fromStartEnd(secs(2.2), secs(2.6));
        plate.audio = RedactionAudio::Bleep;
        plate.audioRanges = {TimeRange::fromStartEnd(secs(2.3), secs(2.4)),
                             TimeRange::fromStartEnd(secs(0.2), secs(0.3))};
        plate.region = pathMask(0, 2.2, 2.6);
        s.redactions.push_back(plate);
        Redaction early;
        early.id = s.ids.allocate<RedactionId>();
        early.media = p.h264;
        early.name = "Before the used part";
        early.source = TimeRange::fromStartEnd(secs(0.1), secs(0.2));
        s.redactions.push_back(early);
        for (MediaPtr& m : s.media) {
            if (m->id != p.h264) {
                continue;
            }
            auto item = std::make_shared<MediaItem>(*m);
            item->masterEffects.push_back(blurWith(s.ids.allocate<EffectInstanceId>(), 2.4, pathMask(1, 2.4, 2.9)));
            Marker sponsor;
            sponsor.start = secs(2.4);
            sponsor.duration = secs(0.2);
            sponsor.name = "Sponsor";
            Marker intro;
            intro.start = secs(0.5);
            intro.name = "Intro";
            item->markers = {intro, sponsor};
            item->markIn = secs(2.1);
            item->markOut = secs(2.9);
            item->info.startTimecode = secs(3600.0 * 10.0); // the file's own timecode (10:00:00:00)
            item->timecode = MediaTimecode{secs(3600.0 * 23.0 + 3599.0), "A001"}; // an override just before midnight
            m = item;
        }
        TranscriptEdits edits;
        edits.media = p.h264;
        edits.audioStream = 0;
        edits.corrections.push_back({TimeRange::fromStartEnd(secs(0.2), secs(0.3)), "early", "early"});
        edits.corrections.push_back({TimeRange::fromStartEnd(secs(2.3), secs(2.4)), "teh", "the"});
        s.transcriptEdits.push_back(edits);
    });
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Trimmed";
    options.trim = true;
    options.handles = Ticks{kTicksPerSecond / 2};
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->trimmedFiles == 2);

    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    const ProjectState& state = *reopened.snapshot();
    const Ticks shift = clipOf(original, p.sequence, p.h264Clip).sourceIn - clipOf(state, p.sequence, p.h264Clip).sourceIn;
    INFO("the copy starts " << shift.toSeconds() << " s earlier");
    REQUIRE(shift > secs(0.6)); // 1.5 s (the 0.5 s handle before the 2 s In), on a keyframe at or before it
    const auto earlier = [&](double seconds) { return secs(seconds) - shift; };

    // The clip's effect: parameter keys and mask path keys and mask parameters.
    const EffectStack& clipFx = clipOf(state, p.sequence, p.h264Clip).effects();
    REQUIRE(clipFx.size() == 1);
    CHECK(std::get<Curve>(clipFx[0].params[0].value.data).keys[0].time == earlier(2.2));
    REQUIRE(clipFx[0].masks.size() == 1);
    CHECK(clipFx[0].masks[0].path[0].time == earlier(2.2));
    CHECK(clipFx[0].masks[0].path[1].time == earlier(2.8));
    CHECK(std::get<Curve>(clipFx[0].masks[0].find("opacity")->data).keys[1].time == earlier(2.8));

    // The redaction: source, the audio range that is left (the one before the file went) and its region's keys. The
    // one wholly before the trimmed part is gone.
    REQUIRE(state.redactions.size() == 1);
    const Redaction& plate = state.redactions[0];
    CHECK(plate.name == "Licence plate");
    CHECK(plate.source.start == earlier(2.2));
    CHECK(plate.source.end() == earlier(2.6));
    REQUIRE(plate.audioRanges.size() == 1);
    CHECK(plate.audioRanges[0].start == earlier(2.3));
    CHECK(plate.audio == RedactionAudio::Bleep);
    CHECK(plate.region.path[0].time == earlier(2.2));
    CHECK(std::get<Curve>(plate.region.find("opacity")->data).keys[0].time == earlier(2.2));

    // The item: Master Clip Effects, markers (the early one is cut), marks.
    const MediaItem* item = state.findMedia(p.h264);
    REQUIRE(item);
    REQUIRE(item->masterEffects.size() == 1);
    CHECK(std::get<Curve>(item->masterEffects[0].params[0].value.data).keys[0].time == earlier(2.4));
    CHECK(item->masterEffects[0].masks[0].path[1].time == earlier(2.9));
    REQUIRE(item->markers.size() == 1);
    CHECK(item->markers[0].name == "Sponsor");
    CHECK(item->markers[0].start == earlier(2.4));
    REQUIRE(item->markIn.has_value());
    CHECK(*item->markIn == earlier(2.1));
    CHECK(*item->markOut == earlier(2.9));

    // The first frame of the copy is `shift` later than the original's, so are the file's start timecode and the Start
    // Timecode override (wrapping at 24 hours); the reel name stays.
    REQUIRE(item->info.startTimecode.has_value());
    CHECK(*item->info.startTimecode == secs(3600.0 * 10.0) + shift);
    REQUIRE(item->timecode.has_value());
    REQUIRE(item->timecode->start.has_value());
    CHECK(item->timecode->start->value == (secs(3600.0 * 23.0 + 3599.0) + shift).value % (kMaxStartTimecode.value + 1));
    CHECK(*item->timecode->start < secs(3600.0)); // wrapped past midnight
    CHECK(item->timecode->reelName == "A001");

    // Transcript edits.
    REQUIRE(state.transcriptEdits.size() == 1);
    REQUIRE(state.transcriptEdits[0].corrections.size() == 1);
    CHECK(state.transcriptEdits[0].corrections[0].text == "the");
    CHECK(state.transcriptEdits[0].corrections[0].source.start == earlier(2.3));
}

// Card #89: a subclip and its master share their redactions and Master Clip Effects, so they must be cut alike.
TEST_CASE("Collect Files cuts a subclip and its master at the same time when they share redactions (089)", "[editor][capsule]") {
    using namespace trimc2;
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-trim-subclip");
    EditSession session;
    ProjectDocument doc(session);
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    const SequenceId sequence = created->id;
    session.reset(std::move(created->state));
    fs::create_directories(dir / "source" / "media");
    fs::copy_file(testMedia("h264_1080p60_aac.mp4"), dir / "source" / "media" / "interview.mp4", fs::copy_options::overwrite_existing);
    const fs::path file = dir / "source" / "project.wabcut";
    REQUIRE(doc.saveAs(file));
    const MediaId master = importOne(session, dir / "source" / "media" / "interview.mp4", file);
    MediaId sub;
    auto made = session.apply("Make subclip", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto sc = edit::makeSubclip(s, master, TimeRange::fromStartEnd(secs(2), secs(3)), "Answer");
        if (!sc) {
            return std::unexpected(sc.error());
        }
        sub = sc->id;
        return sc->state;
    });
    REQUIRE(made);
    // Only the subclip is on the timeline; the master has the effects and a redaction.
    const ClipId clip = drop(session, sequence, sub, Ticks{});
    editState(session, [&](ProjectState& s) {
        for (MediaPtr& m : s.media) {
            if (m->id == master) {
                auto item = std::make_shared<MediaItem>(*m);
                item->masterEffects.push_back(blurWith(s.ids.allocate<EffectInstanceId>(), 2.4, pathMask(1, 2.4, 2.9)));
                m = item;
            }
        }
        Redaction r;
        r.id = s.ids.allocate<RedactionId>();
        r.media = master;
        r.source = TimeRange::fromStartEnd(secs(2.2), secs(2.6));
        s.redactions.push_back(r);
    });
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Subclips";
    options.trim = true;
    options.handles = Ticks{kTicksPerSecond / 2};
    auto result = collectProject(original, file, options);
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result);
    // The master is in the capsule although no clip plays it: its Master Clip Effects belong to the subclip's clips.
    CHECK(result->mediaFiles == 2);
    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(result->projectFile));
    const ProjectState& state = *reopened.snapshot();
    const MediaItem* masterAfter = state.findMedia(master);
    const MediaItem* subAfter = state.findMedia(sub);
    REQUIRE(masterAfter);
    REQUIRE(subAfter);
    REQUIRE(subAfter->subclip.has_value());
    REQUIRE(masterAfter->masterEffects.size() == 1);
    REQUIRE(state.redactions.size() == 1);
    const Ticks shift = clipOf(original, sequence, clip).sourceIn - clipOf(state, sequence, clip).sourceIn;
    CHECK(shift > Ticks{});
    // One media time for both items: the redaction, the master's effect keys and the subclip's range moved alike.
    CHECK(state.redactions[0].source.start == secs(2.2) - shift);
    CHECK(std::get<Curve>(masterAfter->masterEffects[0].params[0].value.data).keys[0].time == secs(2.4) - shift);
    CHECK(subAfter->subclip->range.start == secs(2) - shift);
    CHECK(subAfter->subclip->range.end() == secs(3) - shift);
    // The subclip's clip still sits inside its (restricted) range.
    CHECK(clipOf(state, sequence, clip).sourceIn >= subAfter->subclip->range.start);
}

TEST_CASE("Collect Files copies media with object masks whole and says so (089)", "[editor][capsule]") {
    using namespace trimc2;
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-trim-matte");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    editState(session, [&](ProjectState& s) {
        Redaction r;
        r.id = s.ids.allocate<RedactionId>();
        r.media = p.h264;
        r.source = TimeRange::fromStartEnd(secs(2.2), secs(2.6));
        r.region.shape = MaskShape::Matte;
        MaskMatte matte;
        matte.track = "track-1";
        matte.media = p.h264;
        matte.source = r.source;
        matte.model = "sam";
        r.region.matte = matte;
        for (const char* key : {"center.x", "center.y", "size.x", "size.y"}) { // the bounding box a matte mask keeps
            r.region.set(key, ParamValue(0.5));
        }
        s.redactions.push_back(r);
    });
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Matte";
    options.trim = true;
    auto r = collectProject(original, p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->trimmedFiles == 1); // the ProRes clip is cut, the H.264 clip with the object mask is not
    CHECK(fs::file_size(r->folder / "Media" / "h264_1080p60_aac.mp4") == fs::file_size(testMedia("h264_1080p60_aac.mp4")));
    bool said = false;
    for (const std::string& note : r->notes) {
        said = said || (note.find("copied whole") != std::string::npos && note.find("object masks") != std::string::npos);
    }
    CHECK(said);
    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    const ProjectState& state = *reopened.snapshot();
    REQUIRE(state.redactions.size() == 1);
    CHECK(state.redactions[0].source == original.redactions[0].source); // not shifted: the file was not cut
    CHECK(clipOf(state, p.sequence, p.h264Clip).sourceIn == secs(2));
}

// Card #172: the dashboard's poster cache.
TEST_CASE("Recovery poster cache: one file per version, stale ones and old ones are swept (172)", "[editor][recovery]") {
    const fs::path dir = workDir("recovery-poster-cache");
    const auto touch = [](const fs::path& file) { std::ofstream(file, std::ios::binary) << "png"; };
    const auto entry = [&](const char* name, long long modifiedTicks) {
        RecoveryEntry e;
        e.file = dir / "project" / name;
        e.summary.loadable = true;
        e.summary.modified = fs::file_time_type(fs::file_time_type::duration(modifiedTicks));
        return e;
    };
    const fs::path posters = dir / "recovery-posters";
    fs::create_directories(posters);
    const RecoveryEntry current = entry("a.wabcut", 1000);
    const RecoveryEntry autosave = entry("a.wabcut.autosave", 2000);
    // The same file under another modification time is another poster (an autosave written again).
    RecoveryEntry resaved = autosave;
    resaved.summary.modified = fs::file_time_type(fs::file_time_type::duration(3000));
    CHECK(recoveryPosterFile(posters, current) != recoveryPosterFile(posters, autosave));
    CHECK(recoveryPosterFile(posters, autosave) != recoveryPosterFile(posters, resaved));
    CHECK(recoveryPosterFile(posters, current).parent_path() == posters);
    CHECK(recoveryPosterFile(posters, current).extension() == ".png");

    touch(recoveryPosterFile(posters, current));
    touch(recoveryPosterFile(posters, autosave)); // out of date: the autosave was written again
    touch(recoveryPosterFile(posters, resaved));
    const RecoveryEntry other = entry("b.wabcut", 5000); // another project's recent version
    touch(recoveryPosterFile(posters, other));
    touch(posters / "notes.txt"); // never ours
    const fs::path ancient = posters / "0123456789abcdef-1.png"; // a version nobody lists, long unused
    touch(ancient);
    fs::last_write_time(ancient, fs::file_time_type::clock::now() - std::chrono::hours(24 * 90));

    // This scan lists a.wabcut and its newest autosave only.
    const std::size_t removed = pruneRecoveryPosters(posters, {current, resaved});
    CHECK(removed == 2); // the superseded autosave poster and the ancient one
    CHECK(fs::exists(recoveryPosterFile(posters, current)));
    CHECK(fs::exists(recoveryPosterFile(posters, resaved)));
    CHECK_FALSE(fs::exists(recoveryPosterFile(posters, autosave)));
    CHECK(fs::exists(recoveryPosterFile(posters, other))); // recent and another project's: stays
    CHECK(fs::exists(posters / "notes.txt"));
    CHECK_FALSE(fs::exists(ancient));
    // Nothing to do the second time.
    CHECK(pruneRecoveryPosters(posters, {current, resaved}) == 0);
    // A folder that is not there is not an error.
    CHECK(pruneRecoveryPosters(dir / "missing", {current}) == 0);
}

// Card 003 (a): the handle length is the user's: 0 to 10 s of footage kept either side of each used range.
TEST_CASE("Collect Files keeps the handle length it is given around each used range (003)", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-handles");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    const ProjectState original = session.stateForSave();

    const auto collect = [&](const char* name, double handleSeconds) {
        CapsuleOptions options;
        options.destination = dir / "out";
        options.name = name;
        options.trim = true;
        options.handles = trimc2::secs(handleSeconds);
        auto r = collectProject(original, p.file, options);
        INFO((r ? std::string() : r.error().message));
        REQUIRE(r);
        REQUIRE(r->verified);
        return *r;
    };
    const auto shiftOf = [&](const CapsuleResult& r) {
        EditSession reopened;
        ProjectDocument copy(reopened);
        REQUIRE(copy.open(r.projectFile));
        return clipOf(original, p.sequence, p.h264Clip).sourceIn - clipOf(*reopened.snapshot(), p.sequence, p.h264Clip).sourceIn;
    };
    const CapsuleResult none = collect("NoHandles", 0.0);
    const CapsuleResult wide = collect("WideHandles", 5.0);
    // The used range is 2 s to 3 s of the clip: with no handle the copy starts at (about) 2 s, with five seconds of handle
    // at the start of the file. A bigger handle is a bigger file.
    CHECK(shiftOf(none) > Ticks::fromSeconds(1));
    CHECK(shiftOf(wide) == Ticks{});
    CHECK(fs::file_size(wide.folder / "Media" / "h264_1080p60_aac.mp4") >
          fs::file_size(none.folder / "Media" / "h264_1080p60_aac.mp4"));
    // The estimate follows the handles, and the manifest records them.
    CapsuleOptions estimateNone;
    estimateNone.destination = dir / "out";
    estimateNone.trim = true;
    estimateNone.handles = Ticks{};
    CapsuleOptions estimateWide = estimateNone;
    estimateWide.handles = Ticks::fromSeconds(5);
    CHECK(capsuleBytesNeeded(original, p.file, estimateWide) > capsuleBytesNeeded(original, p.file, estimateNone));
    std::ifstream manifest(wide.manifest);
    const nlohmann::json json = nlohmann::json::parse(manifest);
    CHECK(json["handlesSeconds"].get<double>() == 5.0);
}

namespace {

/// A second sequence over `media` (from 0 s, the whole item), as an abandoned version would be.
SequenceId addSequenceWith(EditSession& session, MediaId media, const char* name) {
    SequenceId id;
    auto r = session.apply("Add sequence", [&](const ProjectState& state) -> std::expected<ProjectState, Error> {
        edit::SequenceSettings settings;
        settings.name = name;
        auto created = edit::addSequence(state, settings);
        if (!created) {
            return std::unexpected(created.error());
        }
        id = created->id;
        return created->state;
    });
    REQUIRE(r);
    drop(session, id, media, Ticks{});
    return id;
}

} // namespace

// Card 003 (b): archiving only the final cut leaves out every abandoned version's footage.
TEST_CASE("Collect Files archives the chosen sequences and what they play (003)", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-sequences");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    // The abandoned version plays the sound file only it uses; the final cut (p.sequence) plays the two clips.
    const SequenceId abandoned = addSequenceWith(session, p.unused, "Abandoned");
    // Nest the final cut's ProRes clip: the final cut now plays a sequence of its own, the nest holds the ProRes.
    const ClipId proresClip = session.snapshot()->findSequence(p.sequence)->videoTracks[0]->clips[1].id;
    auto nested = edit::nestClips(*session.snapshot(), p.sequence, std::vector<ClipId>{proresClip}, "Nest");
    REQUIRE(nested);
    REQUIRE(session.commit("Nest", nested->edit.state));
    const SequenceId nest = nested->nested;
    REQUIRE(doc.save());
    const ProjectState original = session.stateForSave();

    // The closure: the final cut plays the nest; the abandoned version plays nothing; a nest alone is just itself.
    CHECK(capsuleSequences(original, {p.sequence}) == std::vector<SequenceId>{p.sequence, nest});
    CHECK(capsuleSequences(original, {abandoned}) == std::vector<SequenceId>{abandoned});
    CHECK(capsuleSequences(original, {nest}) == std::vector<SequenceId>{nest});
    CHECK(capsuleSequences(original, {SequenceId{99999}}).empty());

    // Everything (no list): all three sequences, and the sound file the abandoned version uses.
    CapsuleOptions all;
    all.destination = dir / "out";
    all.name = "All";
    auto everything = collectProject(original, p.file, all);
    INFO((everything ? std::string() : everything.error().message));
    REQUIRE(everything);
    CHECK(everything->sequences.size() == 3);
    CHECK(everything->mediaFiles == 3);

    // Only the final cut: it and its nest, the two clips they play, not the abandoned version nor its sound file.
    CapsuleOptions chosen = all;
    chosen.name = "FinalOnly";
    chosen.sequences = {p.sequence};
    CHECK(capsuleBytesNeeded(original, p.file, chosen) < capsuleBytesNeeded(original, p.file, all));
    // capsuleMedia is the set of media collectProject copies - what per-file extras (analysis sidecars) are limited to.
    CHECK(capsuleMedia(original, p.file, all).size() == 3);
    const std::vector<MediaId> finalMedia = capsuleMedia(original, p.file, chosen);
    CHECK(finalMedia.size() == 2);
    CHECK(std::ranges::count(finalMedia, p.unused) == 0);
    CHECK(capsuleMedia(original, p.file, [&] {
              CapsuleOptions o = chosen;
              o.sequences = {abandoned};
              return o;
          }()) == std::vector<MediaId>{p.unused});
    CHECK(capsuleMedia(original, p.file, [&] {
              CapsuleOptions o = chosen;
              o.excludeUnused = false; // unused media of the project's media list come along
              return o;
          }()).size() == 3);
    auto r = collectProject(original, p.file, chosen);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->verified);
    CHECK(r->mediaFiles == 2);
    CHECK_FALSE(fs::exists(r->folder / "Media" / "sine_48k.wav"));
    REQUIRE(r->sequences.size() == 2);
    CHECK(std::ranges::count(r->sequences, std::string("Abandoned")) == 0);
    CHECK(std::ranges::count(r->sequences, std::string("Nest")) == 1);
    std::ifstream manifestFile(r->manifest);
    const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
    REQUIRE(manifest["sequences"].size() == 2);

    // The capsule opens elsewhere with only those sequences, nothing offline, the nest still played by the final cut.
    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(r->projectFile));
    CHECK(copy.offlineMedia().empty());
    CHECK(reopened.snapshot()->sequences.size() == 2);
    CHECK(reopened.snapshot()->findSequence(abandoned) == nullptr);
    CHECK(reopened.snapshot()->findSequence(nest) != nullptr);
    CHECK(reopened.snapshot()->media.size() == 2);

    // Only the abandoned one: just its sound file.
    CapsuleOptions old = all;
    old.name = "OldOnly";
    old.sequences = {abandoned};
    auto o = collectProject(original, p.file, old);
    REQUIRE(o);
    CHECK(o->mediaFiles == 1);
    CHECK(fs::exists(o->folder / "Media" / "sine_48k.wav"));

    // A list that names no sequence of the project is refused (never "everything"), and nothing is left behind.
    CapsuleOptions none = all;
    none.name = "Nothing";
    none.sequences = {SequenceId{99999}};
    const auto refused = collectProject(original, p.file, none);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(fs::exists(dir / "out" / "Nothing"));
}

// ---- Layout of Media/ (0.7, card 076) ---------------------------------------------------------------------------------

namespace {

/// Puts the bins of `bins` ({bin name, parent index or -1}) in the project and each media item of `homes`
/// ({media, bin index}) into its bin.
void makeBins(EditSession& session, const std::vector<std::pair<std::string, int>>& bins,
              const std::vector<std::pair<MediaId, int>>& homes) {
    REQUIRE(session.apply("Bins", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        ProjectState state = s;
        std::vector<BinId> ids;
        for (const auto& [name, parent] : bins) {
            auto added = edit::addBin(state, name, parent < 0 ? BinId{} : ids[static_cast<std::size_t>(parent)]);
            if (!added) {
                return std::unexpected(added.error());
            }
            state = std::move(added->state);
            ids.push_back(added->id);
        }
        for (const auto& [media, bin] : homes) {
            const std::vector<MediaId> one{media};
            auto moved = edit::moveToBin(state, ids[static_cast<std::size_t>(bin)], {}, one, {});
            if (!moved) {
                return std::unexpected(moved.error());
            }
            state = std::move(*moved);
        }
        return state;
    }));
}

/// Every file of a capsule folder, relative and '/'-separated.
std::set<std::string> filesUnder(const fs::path& folder) {
    std::set<std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            out.insert(entry.path().lexically_relative(folder).generic_string());
        }
    }
    return out;
}

} // namespace

TEST_CASE("Collect Files lays the media out in the project's bins (076)", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-bins");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    // "Interviews" > "Day 1" holds the H.264 clip, "Interviews" the ProRes one; the WAV is unused.
    makeBins(session, {{"Interviews", -1}, {"Day 1", 0}}, {{p.h264, 1}, {p.prores, 0}});

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Job";
    options.layout = CapsuleLayout::Bins;
    auto r = collectProject(session.stateForSave(), p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->verified);
    CHECK(r->offlineAfterReopen == 0);
    CHECK(r->fingerprintMismatches == 0);
    const std::set<std::string> files = filesUnder(r->folder);
    CHECK(files.contains("Media/Interviews/Day 1/h264_1080p60_aac.mp4"));
    CHECK(files.contains("Media/Interviews/prores_422.mov"));
    CHECK_FALSE(files.contains("Media/h264_1080p60_aac.mp4")); // not flat any more
    {
        std::ifstream in(r->manifest); // (closed before the folder is moved)
        CHECK(nlohmann::json::parse(in)["layout"] == "bins");
    }

    // The capsule reopens elsewhere with the same bins and nothing offline.
    fs::rename(r->folder, dir / "elsewhere");
    EditSession reopened;
    ProjectDocument copy(reopened);
    REQUIRE(copy.open(dir / "elsewhere" / "Job.wabcut"));
    CHECK(copy.offlineMedia().empty());
    CHECK(reopened.snapshot()->bins.size() == 2);
    CHECK(reopened.snapshot()->findMedia(p.h264)->bin == session.snapshot()->findMedia(p.h264)->bin);

    // The default stays flat.
    CapsuleOptions flat = options;
    flat.name = "Flat";
    flat.layout = CapsuleLayout::Flat;
    auto f = collectProject(session.stateForSave(), p.file, flat);
    REQUIRE(f);
    CHECK(filesUnder(f->folder).contains("Media/h264_1080p60_aac.mp4"));
}

TEST_CASE("Collect Files keeps the folders the media came from below the folder they share (076)", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-folders");
    const fs::path footage = dir / "source" / "Footage";
    fs::create_directories(footage / "Day 1");
    fs::create_directories(footage / "Day 2" / "Cam A");
    fs::copy_file(testMedia("h264_1080p60_aac.mp4"), footage / "Day 1" / "h264_1080p60_aac.mp4");
    fs::copy_file(testMedia("prores_422.mov"), footage / "Day 2" / "Cam A" / "prores_422.mov");
    fs::copy_file(testMedia("sine_48k.wav"), footage / "sine_48k.wav");
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    const SequenceId sequence = created->id;
    EditSession session;
    ProjectDocument doc(session);
    session.reset(std::move(created->state));
    const fs::path file = dir / "source" / "project.wabcut";
    REQUIRE(doc.saveAs(file));
    const MediaId a = importOne(session, footage / "Day 1" / "h264_1080p60_aac.mp4", file);
    const MediaId b = importOne(session, footage / "Day 2" / "Cam A" / "prores_422.mov", file);
    const MediaId c = importOne(session, footage / "sine_48k.wav", file);
    drop(session, sequence, a, Ticks{});
    drop(session, sequence, b, Ticks::fromSeconds(1));
    drop(session, sequence, c, Ticks::fromSeconds(2));
    REQUIRE(doc.save());

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Job";
    options.layout = CapsuleLayout::SourceFolders;
    auto r = collectProject(session.stateForSave(), file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->verified);
    CHECK(r->offlineAfterReopen == 0);
    const std::set<std::string> files = filesUnder(r->folder);
    CHECK(files.contains("Media/Day 1/h264_1080p60_aac.mp4"));
    CHECK(files.contains("Media/Day 2/Cam A/prores_422.mov"));
    CHECK(files.contains("Media/sine_48k.wav")); // in the shared folder itself
    std::ifstream in(r->manifest);
    CHECK(nlohmann::json::parse(in)["layout"] == "folders");
}

TEST_CASE("Collect Files folder names from bins can never leave Media or collide (076)", "[editor][capsule]") {
    if (!haveClips()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("capsule-hostile-bins");
    EditSession session;
    ProjectDocument doc(session);
    const Project p = makeProject(session, doc, dir / "source");
    // A bin named like a path out of the capsule; a reserved device name holding two bins that differ only by case.
    makeBins(session, {{"../../escape", -1}, {"CON", -1}, {"Day 1", 1}, {"day 1", 1}},
             {{p.prores, 0}, {p.h264, 2}, {p.unused, 3}});

    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Job";
    options.layout = CapsuleLayout::Bins;
    options.excludeUnused = false; // the WAV goes too, into the "day 1" bin (beside the "Day 1" one)
    auto r = collectProject(session.stateForSave(), p.file, options);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    CHECK(r->verified);
    CHECK(r->offlineAfterReopen == 0);
    // Nothing but the capsule folder was made in the destination, and nothing in it has a ".." part.
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(dir / "out")) {
        ++entries;
        CHECK(entry.path().filename() == "Job");
    }
    CHECK(entries == 1);
    CHECK_FALSE(fs::exists(dir / "escape"));
    const std::set<std::string> files = filesUnder(r->folder);
    for (const std::string& f : files) {
        CHECK(f.find("/../") == std::string::npos);
    }
    CHECK(files.contains("Media/.._.._escape/prores_422.mov"));
    CHECK(files.contains("Media/CON_/Day 1/h264_1080p60_aac.mp4"));
    CHECK(files.contains("Media/CON_/Day 1/sine_48k.wav")); // "day 1" shares the folder "Day 1" made first
}
