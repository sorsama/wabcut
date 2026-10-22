// 0.2 image sequences (model/ImageSequence.h, editor::describeImageSequence, media.importImageSequence): numbered PNG
// stills written here with Qt become one video item at the chosen rate; it decodes one still per frame through the
// image2 demuxer, stays online through the media check (its first frame stands for it), and the command adds it as one
// undoable step. Malformed sequence paths (untrusted project text) are refused.

#include <wabcut/command/Engine.h>
#include <wabcut/command/ImageSequenceServices.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <QImage>
#include <QString>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace wabcut;
namespace fs = std::filesystem;

namespace {

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/// shot_<number>.png, 64x36, every pixel grey `level`.
void writeStill(const fs::path& dir, const std::string& name, int level) {
    QImage picture(64, 36, QImage::Format_RGB32);
    picture.fill(qRgb(level, level, level));
    REQUIRE(picture.save(QString::fromStdWString((dir / name).wstring()), "PNG"));
}

/// Twelve stills shot_0001..shot_0012 (grey 20, 40, ... 240), then a gap and shot_0014.
fs::path writeSequence(const char* name) {
    const fs::path dir = workDir(name);
    for (int i = 1; i <= 12; ++i) {
        writeStill(dir, "shot_" + std::string(i < 10 ? "000" : "00") + std::to_string(i) + ".png", 20 * i);
    }
    writeStill(dir, "shot_0014.png", 10);
    return dir;
}

} // namespace

TEST_CASE("Image sequence paths round trip and malformed ones are refused", "[image-sequence]") {
    const fs::path dir = writeSequence("image_sequence_paths");
    auto found = findImageSequence(dir / "shot_0001.png", Rate{24, 1});
    REQUIRE(found);
    CHECK(found->count == 12); // stops at the gap before shot_0014
    CHECK(found->first == 1);
    CHECK(found->digits == 4);
    CHECK(found->prefix == "shot_");
    CHECK(found->suffix == ".png");

    const std::string path = imageSequencePath(*found);
    CHECK(path.ends_with("shot_%04d.png?start=1&rate=24:1"));
    const auto parsed = parseImageSequencePath(path);
    REQUIRE(parsed);
    CHECK(parsed->firstFramePath() == found->firstFramePath());
    CHECK(parsed->frameRate == Rate{24, 1});
    CHECK(parsed->digits == 4);
    CHECK(mediaFileExists(pathFromUtf8(path)));
    CHECK(mediaFileToCheck(pathFromUtf8(path)) == found->firstFramePath());

    // From a later frame: that frame onwards.
    auto later = findImageSequence(dir / "shot_0005.png", Rate{30000, 1001});
    REQUIRE(later);
    CHECK(later->first == 5);
    CHECK(later->count == 8);

    // '%' in a name is doubled in the pattern and comes back as one.
    ImageSequenceSpec percent;
    percent.directory = dir / "100%";
    percent.prefix = "a%b_";
    percent.suffix = ".tif";
    percent.digits = 3;
    percent.first = 7;
    percent.frameRate = Rate{25, 1};
    const auto back = parseImageSequencePath(imageSequencePath(percent));
    REQUIRE(back);
    CHECK(back->prefix == "a%b_");
    CHECK(back->firstFramePath() == percent.firstFramePath());

    CHECK_FALSE(parseImageSequencePath(pathToUtf8(dir / "shot_0001.png")));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d.png?start=1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d.png?start=1&rate=0:1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d.png?start=1&rate=5000:1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d.png?start=-1&rate=24:1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d.png?start=99999999999&rate=24:1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d_%04d.png?start=1&rate=24:1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%s.png?start=1&rate=24:1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d.txt?start=1&rate=24:1"));
    CHECK_FALSE(parseImageSequencePath("C:/x/shot_%04d/a.png?start=1&rate=24:1"));
    CHECK_FALSE(parseImageSequencePath(std::string(2000, 'a') + "%04d.png?start=1&rate=24:1"));

    CHECK(findImageSequence(dir / "missing_0001.png", Rate{24, 1}).error().code == ErrorCode::NotFound);
    CHECK(findImageSequence(dir / "shot_0001.png", Rate{0, 1}).error().code == ErrorCode::InvalidArgument);
    writeStill(dir, "poster.png", 128);
    CHECK(findImageSequence(dir / "poster.png", Rate{24, 1}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Numbered PNG stills import as one video item that decodes a still per frame", "[image-sequence]") {
    const fs::path dir = writeSequence("image_sequence_import");
    auto item = editor::describeImageSequence(dir / "shot_0001.png", Rate{24, 1});
    REQUIRE(item);
    CHECK(item->name == "shot_0001.png");
    CHECK_FALSE(item->info.isStillImage);
    REQUIRE(item->info.video.size() == 1);
    CHECK(item->info.audio.empty());
    CHECK(item->info.video.front().frameRate == Rate{24, 1});
    CHECK(item->info.video.front().frameCount == 12);
    CHECK(item->info.video.front().width == 64);
    CHECK(item->sourceExtent({StreamKind::Video, 0}) == Ticks{kTicksPerSecond / 2});
    CHECK(item->locator.fingerprint.isKnown());

    // Frame k is shot_<k+1>: grey 20 (k + 1).
    auto decoder = media::VideoDecoder::open(item->locator.path);
    REQUIRE(decoder);
    int previous = -1;
    for (const int k : {0, 5, 11}) {
        auto frame = (*decoder)->frameAt(Ticks{kTicksPerSecond * k / 24});
        REQUIRE(frame);
        REQUIRE(frame->cpu());
        const int value = frame->cpu()->planes[0].data[0];
        CHECK(value > previous); // later frames are lighter
        previous = value;
        if (frame->format == media::PixelFormat::RGBA8) {
            CHECK(value == 20 * (k + 1));
        }
    }

    // The media check finds it by its first frame, and reports it offline once that is gone.
    const fs::path project = dir / "p.wabcut";
    CHECK_FALSE(editor::checkMediaOnDisk(*item, project).first.locator.offline);
    fs::remove(dir / "shot_0001.png");
    CHECK(editor::checkMediaOnDisk(*item, project).first.locator.offline);
}

TEST_CASE("Interpret Footage conforms an image sequence's clips like any footage (p02 integration)", "[image-sequence]") {
    // p02-editing's Interpret Footage on p02-project's image sequence item: 24 fps stills assumed at 12 fps play every
    // still for two sequence frames (speed 1/2), the clip twice as long.
    const fs::path dir = writeSequence("image_sequence_interpret");
    auto described = editor::describeImageSequence(dir / "shot_0001.png", Rate{24, 1});
    REQUIRE(described);
    ProjectState s;
    MediaItem item = *described;
    item.id = s.ids.allocate<MediaId>();
    s.media.push_back(std::make_shared<const MediaItem>(item));
    auto seq = std::make_shared<Sequence>();
    seq->id = s.ids.allocate<SequenceId>();
    seq->name = "Stills";
    seq->frameRate = Rate{24, 1};
    seq->width = 64;
    seq->height = 36;
    Clip clip;
    clip.id = s.ids.allocate<ClipId>();
    clip.media = item.id;
    clip.timelineRange = {Ticks{}, Ticks{kTicksPerSecond / 2}};
    Track track;
    track.id = s.ids.allocate<TrackId>();
    track.clips = ClipList(std::vector<Clip>{clip});
    seq->videoTracks = {std::make_shared<const Track>(std::move(track))};
    seq->duration = Ticks{kTicksPerSecond * 2};
    s.sequences.push_back(seq);

    MediaInterpretation in;
    in.frameRate = Rate{12, 1};
    auto r = edit::interpretFootage(s, std::span(&item.id, 1), in);
    REQUIRE(r);
    CHECK(edit::conformSpeed(*r->state.findMedia(item.id)) == Speed{1, 2});
    const Clip& conformed = r->state.findSequence(seq->id)->videoTracks.at(0)->clips.front();
    CHECK(conformed.speed == Speed{1, 2});
    CHECK(conformed.timelineRange.duration == Ticks{kTicksPerSecond});
}

TEST_CASE("media.importImageSequence adds the sequence as one undo step", "[image-sequence]") {
    const fs::path dir = writeSequence("image_sequence_command");
    EditSession session;
    command::CommandRequest request;
    request.command = "media.importImageSequence";
    request.params = command::Json{{"path", pathToUtf8(dir / "shot_0001.png")}, {"frameRate", 23.976}};

    {
        // A host that cannot read media refuses it.
        command::CommandEngine bare(session);
        const command::CommandResult refused = bare.execute(request);
        REQUIRE(refused.error);
        CHECK(refused.error->code == command::ErrorCode::EditRejected);
    }

    command::EnginePolicy policy;
    policy.services = std::make_shared<command::ImageSequenceServices>(
        [](const fs::path& frame, Rate rate) { return editor::describeImageSequence(frame, rate); });
    command::CommandEngine engine(session, command::builtinCommands(), std::move(policy));
    const command::CommandResult result = engine.execute(request);
    REQUIRE_FALSE(result.error);
    REQUIRE(result.createdIds.size() == 1);
    CHECK(result.data["frames"] == 12);
    CHECK(result.data["frameRate"]["num"] == 24000);
    CHECK(result.data["frameRate"]["den"] == 1001);
    const ProjectSnapshot state = session.snapshot();
    REQUIRE(state->media.size() == 1);
    CHECK(state->media.front()->info.video.front().frameRate == Rate{24000, 1001});
    CHECK(parseImageSequencePath(state->media.front()->locator.path).has_value());
    REQUIRE(validate(*state));

    // The project file keeps it, and it is online again after a reload (found by its first frame).
    auto json = io::toJson(*state);
    REQUIRE(json);
    auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    REQUIRE(loaded->media.size() == 1);
    CHECK(*loaded->media.front() == *state->media.front());
    const ProjectState checked = editor::withMediaChecked(*loaded, dir / "p.wabcut");
    CHECK_FALSE(checked.media.front()->locator.offline);
    CHECK(checked.media.front()->locator.path == state->media.front()->locator.path);

    CHECK(command::imageSequenceRate(29.97) == Rate{30000, 1001});
    CHECK(command::imageSequenceRate(25) == Rate{25, 1});
    CHECK(command::imageSequenceRate(12.5) == Rate{25, 2});

    REQUIRE(session.undo());
    CHECK(session.snapshot()->media.empty());

    // A file that is not numbered fails without changing the project.
    request.params["path"] = pathToUtf8(dir / "nothing.png");
    CHECK(engine.execute(request).error);
    CHECK(session.snapshot()->media.empty());
}
